/*
 * Copyright © 2025 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * FreeRTOS application integration for the Deluge firmware.
 *
 * Phase 6b architecture:
 * - Audio task at FreeRTOS priority 7 — pure DSP, drains voice event queue
 * - Sequencer task at priority 6 — tick advancement, event scheduling, UI graphics
 * - App task at FreeRTOS priority 3 — runs cooperative scheduler, UI
 * - Storage task at priority 2 — song/preset load/save, XML parsing
 * - IRQ handler bridges to existing GIC dispatch via vApplicationFPUSafeIRQHandler
 */

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "processing/engines/audio_engine.h"
#include "semphr.h"
#include "storage/storage_task.h"
#include "task.h"

/* --------------------------------------------------------------------------
 * IRQ bridge: FreeRTOS IRQ handler → existing Deluge GIC dispatch
 *
 * FreeRTOS's FreeRTOS_IRQ_Handler (portASM.S) reads ICCIAR, saves context,
 * calls vApplicationFPUSafeIRQHandler(icciar), writes ICCEOIR, then checks
 * ulPortYieldRequired for context switch. We bridge to the existing Deluge
 * interrupt dispatch which handles all registered ISRs.
 * -------------------------------------------------------------------------- */
extern "C" {
void INTC_Handler_Interrupt(uint32_t icciar);

void vApplicationFPUSafeIRQHandler(uint32_t ulICCIAR) {
	INTC_Handler_Interrupt(ulICCIAR);
}
}

/* --------------------------------------------------------------------------
 * Static task allocations
 * -------------------------------------------------------------------------- */

/* App task: runs cooperative scheduler for all non-audio tasks. 32KB stack. */
static StaticTask_t sAppTaskTCB;
static StackType_t sAppTaskStack[8192];

/* Audio task: dedicated high-priority task. 16KB stack. */
static StaticTask_t sAudioTaskTCB;
static StackType_t sAudioTaskStack[4096];

/* Sequencer task: tick advancement, event scheduling, MIDI/CV I/O, UI. 16KB stack. */
static StaticTask_t sSequencerTaskTCB;
static StackType_t sSequencerTaskStack[4096];

/* Storage task: song/preset load/save, XML parsing, sample file indexing. 64KB stack.
 * Needs extra room for: StorageCommand (~780B local), deep FatFS/XML parse call chain,
 * and f_read's internal buffer operations. */
static StaticTask_t sStorageTaskTCB;
static StackType_t sStorageTaskStack[16384];

/* Global handles for task management */
TaskHandle_t audioTaskHandle = nullptr;
TaskHandle_t sequencerTaskHandle = nullptr;

/* Idle task (required when configSUPPORT_STATIC_ALLOCATION=1). */
static StaticTask_t sIdleTaskTCB;
static StackType_t sIdleTaskStack[configMINIMAL_STACK_SIZE];

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t** ppxIdleTaskTCBBuffer, StackType_t** ppxIdleTaskStackBuffer,
                                              uint32_t* pulIdleTaskStackSize) {
	*ppxIdleTaskTCBBuffer = &sIdleTaskTCB;
	*ppxIdleTaskStackBuffer = sIdleTaskStack;
	*pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName) {
	(void)xTask;
	(void)pcTaskName;
	extern void freezeWithError(const char* errmsg);
	freezeWithError("STAK");
}

/* --------------------------------------------------------------------------
 * Audio task: interrupt-driven via SSI TX DMA ping-pong descriptors.
 *
 * Two DMA descriptors (A and B) each cover half the 128-sample TX buffer.
 * When each 64-sample half-buffer transfer completes (~1.45ms at 44.1kHz),
 * the DMA interrupt fires and notifies the audio task. The audio task wakes,
 * renders into the half that just finished playing, then sleeps until the
 * next interrupt.
 *
 * This replaces the polling approach (vTaskDelayUntil + getTxBufferCurrentPlace)
 * with deterministic hardware-driven scheduling:
 * - No wasted CPU cycles polling DMA position
 * - Precise timing — the task wakes exactly when samples are needed
 * - CPU is free for cluster loading, UI, and other tasks between renders
 * -------------------------------------------------------------------------- */
#include "OSLikeStuff/timers_interrupts/timers_interrupts.h"
#include "RZA1/intc/devdrv_intc.h"
#include "RZA1/system/iodefines/dmac_iodefine.h"
#include "definitions.h"
#include "drivers/dmac/dmac.h"
#include "drivers/ssi/ssi.h"
#include "processing/engines/voice_event_queue.h"

extern "C" {
#include "drivers/sd/sd_async.h"
}

namespace AudioEngine {
void routine();
}

/* DMA interrupt handler for SSI TX (channel 6).
 * Called when each half-buffer DMA transfer completes.
 * Notifies the audio task to render the next half. */
static void ssiTxDmaISR(uint32_t intSense) {
	(void)intSense;

	/* The DMAC automatically chains to the next linked descriptor and
	 * continues transferring. We don't need to restart the channel.
	 * Just clear the transfer-end status bit so the next interrupt fires. */
	DMACn(SSI_TX_DMA_CHANNEL).CHCTRL_n = 0x00000040uL; /* CLRTC: clear transfer complete flag */

	/* Toggle which half just completed */
	ssiTxHalfComplete ^= 1;

	/* Notify both audio and sequencer tasks.
	 * Audio (priority 7) preempts and renders immediately using voice state
	 * prepared by the sequencer's *previous* pass. The sequencer (priority 6)
	 * runs after audio finishes, preparing voice state for the *next* buffer.
	 * They naturally take turns — no concurrent access to voice state. */
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	if (audioTaskHandle != nullptr) {
		vTaskNotifyGiveFromISR(audioTaskHandle, &xHigherPriorityTaskWoken);
	}
	if (sequencerTaskHandle != nullptr) {
		vTaskNotifyGiveFromISR(sequencerTaskHandle, &xHigherPriorityTaskWoken);
	}
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* Initialize the DMA interrupt for SSI TX.
 * Must be called after the scheduler starts (interrupt priority must be
 * in the FreeRTOS managed zone). */
void ssiTxDmaInterruptInit(void) {
	/* DMA channel 6 interrupt ID = INTC_ID_DMAINT0 + 6 = 47
	 * Priority 19: within FreeRTOS managed zone (>= configMAX_API_CALL_INTERRUPT_PRIORITY=18)
	 * so xTaskNotifyGiveFromISR can be called safely from the ISR. */
	setupAndEnableInterrupt(ssiTxDmaISR, DMA_INTERRUPT_0 + SSI_TX_DMA_CHANNEL, 19);

	/* Clear interrupt mask on the DMA channel to allow interrupts */
	DMACn(SSI_TX_DMA_CHANNEL).CHCTRL_n |= 0x00020000uL; /* CLRINTMSK */
}

/* Audio CPU load measurement — uses OSTM0 (33.33 MHz free-running counter).
 * Tracks worst-case render time over a measurement window. */
extern "C" {
#include "RZA1/ostm/ostm.h"
}

static void audioTaskFunction(void* pvParameters) {
	(void)pvParameters;

	/* Initialize the DMA interrupt now that the scheduler is running */
	ssiTxDmaInterruptInit();

	/* Start the async SD layer — all SD operations now go through
	 * the ISR state machine instead of synchronous sd_read_sect/sd_write_sect */
	sdAsyncStart();

	for (;;) {
		/* Block until DMA half-buffer transfer completes.
		 * The ISR fires every 64 samples (~1.45ms at 44.1kHz).
		 * The audio task is a pure DSP consumer: it drains the voice event
		 * queue, renders all active voices/effects, and writes to the DMA buffer.
		 * Tick logic and I/O are handled by the sequencer task. */
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		/* Clean up voices that the audio task marked for deletion during
		 * the previous render cycle. Must happen before processing new
		 * events so that zombie voices don't interfere with noteOn logic. */
		AudioEngine::cleanupDeletedVoices();

		/* Process all pending voice events before rendering.
		 * This is where noteOn/noteOff/expression/kill events from the
		 * sequencer, app, and MIDI tasks get applied to voice state.
		 * The audio task is the sole owner of voices — all modifications
		 * happen here. */
		voiceEventProcessAll();

		AudioEngine::routine();
	}
}

/* --------------------------------------------------------------------------
 * Cluster loading: ISR reads clusters, sequencer post-processes
 * -------------------------------------------------------------------------- */
#include "drivers/sd/sd_async.h"
#include "model/sample/sample.h"
#include "model/sample/sample_cluster.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"

/* ISR callback for async cluster reads. Pushes the completed cluster
 * into the completion ring buffer for the sequencer to post-process. */
static void clusterReadCompletionCallback(int32_t result, void* userData) {
	/* Push to completion ring — sequencer drains via drainClusterCompletions() */
	extern void sdAsyncCompletionPushFromCallback(void* cluster, int32_t result);
	sdAsyncCompletionPushFromCallback(userData, result);
}

/*
 * Feed pending cluster reads from the loading queue to the ISR fast queue.
 * Called from the sequencer task each cycle (~1.45ms).
 */
/* Counters for deterministic cluster pipeline drain.
 * Submitted: incremented when feedClusterReadsToISR hands a cluster to the ISR.
 * Completed: incremented when drainClusterCompletions finishes post-processing.
 * When submitted == completed, the pipeline is fully drained. */
volatile uint32_t clusterReadsSubmitted = 0;
volatile uint32_t clusterReadsCompleted = 0;

static void feedClusterReadsToISR() {
	if (!sdAsyncIsActive()) {
		return;
	}

	while (true) {
		Cluster* cluster = audioFileManager.loadingQueue.getNext();
		if (cluster == nullptr) {
			break;
		}

		Sample* sample = cluster->sample;
		if (sample == nullptr) {
			/* Invalid cluster — skip */
			continue;
		}

		if (cluster->type != Cluster::Type::SAMPLE) {
			continue;
		}

		int32_t clusterIndex = cluster->clusterIndex;
		int32_t numSectors = Cluster::size >> 9;

		/* Handle partial last cluster */
		if (sample->audioDataLengthBytes && sample->audioDataLengthBytes != 0x8FFFFFFFFFFFFFFF) {
			uint32_t audioDataEndPosBytes = sample->audioDataLengthBytes + sample->audioDataStartPosBytes;
			uint32_t startByteThisCluster = clusterIndex << Cluster::size_magnitude;
			int32_t bytesToRead = audioDataEndPosBytes - startByteThisCluster;
			if (bytesToRead <= 0) {
				continue;
			}
			if (bytesToRead < (int32_t)Cluster::size) {
				numSectors = ((bytesToRead - 1) >> 9) + 1;
			}
		}

		LBA_t startSector = sample->clusters.getElement(clusterIndex)->sdAddress;

		/* Add a reason to prevent deallocation during async read */
		cluster->addReason();

		bool enqueued = sdAsyncReadCluster(startSector, (uint8_t*)cluster->data, numSectors,
		                                   clusterReadCompletionCallback, (void*)cluster);

		if (enqueued) {
			clusterReadsSubmitted++;
		}

		if (!enqueued) {
			/* Fast queue full — put it back and try next cycle */
			audioFileManager.removeReasonFromCluster(*cluster, "E033");
			audioFileManager.loadingQueue.enqueueCluster(*cluster, 0xFFFFFFFF);
			break;
		}
	}
}

/*
 * Drain the ISR completion ring buffer and post-process completed clusters.
 * Called from the sequencer task each cycle. Must only be called from the
 * sequencer — the completion ring is SPSC (single consumer).
 */
static void drainClusterCompletions() {
	SdClusterCompletion comp;
	while (sdAsyncCompletionPop(&comp)) {
		Cluster* cluster = (Cluster*)comp.cluster;
		if (comp.result == 0) {
			audioFileManager.postProcessLoadedCluster(*cluster);
		}
		else {
			/* Read error — release the loading reason */
			audioFileManager.removeReasonFromCluster(*cluster, "E033");
		}
		clusterReadsCompleted++;
	}
}

/* --------------------------------------------------------------------------
 * App task: runs existing cooperative scheduler for non-audio tasks
 * -------------------------------------------------------------------------- */
static void appTaskFunction(void* pvParameters) {
	void (*schedulerEntry)(void) = (void (*)(void))pvParameters;
	schedulerEntry(); /* Infinite loop — never returns */
}

/* --------------------------------------------------------------------------
 * Sequencer task: owns tick advancement, event scheduling, MIDI/CV I/O,
 * and UI graphics updates. Wakes on DMA ISR every ~2.9ms (same as audio).
 * Runs at priority 6 — below audio (7), above cluster loader (5).
 *
 * Graphics updates run every ~5th wake-up (~14.5ms), replacing the former
 * FreeRTOS timer daemon approach. The sequencer owns playback position,
 * making it the natural place for playhead/LED updates.
 * -------------------------------------------------------------------------- */
#include "gui/ui/ui.h"

extern "C" {
int32_t uartGetTxBufferSpace(int32_t item);
}

class Song;
extern Song* currentSong;
extern Song* preLoadedSong;

static void graphicsUpdate() {
	if (currentSong == nullptr) {
		return;
	}
	/* Only update if UART has space — matches existing GRAPHICS_ROUTINE behavior.
	 * UART_ITEM_PIC_PADS = 1, kNumBytesInColUpdateMessage = 49 */
	if (uartGetTxBufferSpace(1) > 49) {
		UI* ui = getCurrentUI();
		if (ui != nullptr) {
			ui->graphicsRoutine();
		}
	}
}

/* --------------------------------------------------------------------------
 * Sequencer tick advancement — moved from AudioEngine::tickSongFinalizeWindows.
 *
 * Processes all ticks that fall within the current half-buffer period.
 * Unlike the old code, this does NOT split render windows — the audio task
 * always renders a full 128-sample half-buffer. The sequencer fires all
 * tick events and modifies voice state; the audio task renders the result
 * on its next wake-up (one buffer of latency).
 * -------------------------------------------------------------------------- */
#include "definitions.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/engines/voice_event_queue.h"
#include "storage/cluster/cluster_prefetch.h"

#define TICK_TYPE_SWUNG 1
#define TICK_TYPE_TIMER 2

static constexpr size_t kHalfBufferSamples = SSI_TX_BUFFER_NUM_SAMPLES / 2;

static void sequencerRoutine() {
	/* Poll MIDI/analog clock input */
	playbackHandler.routine();

	if (!playbackHandler.isEitherClockActive()) {
		return;
	}

	/* Process all ticks up to one half-buffer ahead of audioSampleTimer.
	 * No tick dropping — all ticks are processed. The larger buffer (512 samples,
	 * 256-sample halves, 5.8ms per cycle) gives the sequencer enough time
	 * to process all ticks without overrunning. Performance tuning (buffer size,
	 * tick dropping for overload) can be revisited when the port is more complete. */
	uint32_t bufferEnd = AudioEngine::audioSampleTimer + kHalfBufferSamples;

	/* Process all timer and swung ticks within this buffer window. */
	for (;;) {
		/* No safety counter — the original tickSongFinalizeWindows had none.
		 * Each tick advances timeNextTimerTickBig/scheduledSwungTickTime,
		 * guaranteeing eventual termination when timeNextTick >= bufferEnd. */
		int32_t nextTickType = 0;
		uint32_t timeNextTick = AudioEngine::audioSampleTimer + 9999;

		if (playbackHandler.isInternalClockActive()) {
			timeNextTick = playbackHandler.timeNextTimerTickBig >> 32;
			nextTickType = TICK_TYPE_TIMER;
		}

		if (playbackHandler.swungTickScheduled
		    && (int32_t)(playbackHandler.scheduledSwungTickTime - timeNextTick) < 0) {
			timeNextTick = playbackHandler.scheduledSwungTickTime;
			nextTickType = TICK_TYPE_SWUNG;
		}

		/* If next tick is beyond this buffer, we're done */
		if ((int32_t)(timeNextTick - bufferEnd) >= 0) {
			break;
		}

		/* Fire the tick */
		if (nextTickType == TICK_TYPE_TIMER) {
			playbackHandler.actionTimerTick();
		}
		else if (nextTickType == TICK_TYPE_SWUNG) {
			playbackHandler.actionSwungTick();
			playbackHandler.scheduleSwungTick();
		}
	}

	/* Drain all pending clock output ticks for this buffer.
	 * Clock ticks fire at a higher rate than timer/swung ticks (e.g. 24 PPQN),
	 * so multiple clock ticks can fall within a single buffer. Each iteration
	 * advances state, enqueues an AudioEvent if not skipped, and schedules the
	 * next clock tick. The loop runs until no more clock ticks fall within
	 * this buffer window. */
	while (playbackHandler.triggerClockOutTickScheduled) {
		int32_t timeTil = playbackHandler.timeNextTriggerClockOutTick - AudioEngine::audioSampleTimer;
		if (timeTil >= (int32_t)kHalfBufferSamples) {
			break;
		}
		if (playbackHandler.advanceTriggerClockOutTick()) {
			uint16_t offset = static_cast<uint16_t>(timeTil);
			g_audioEventQueue.push(AudioEvent{AudioEvent::Type::TRIGGER_CLOCK_OUT, offset, 0});
		}
		playbackHandler.scheduleTriggerClockOutTick();
	}

	while (playbackHandler.midiClockOutTickScheduled) {
		int32_t timeTil = playbackHandler.timeNextMIDIClockOutTick - AudioEngine::audioSampleTimer;
		if (timeTil >= (int32_t)kHalfBufferSamples) {
			break;
		}
		if (playbackHandler.advanceMIDIClockOutTick()) {
			uint16_t offset = static_cast<uint16_t>(timeTil);
			g_audioEventQueue.push(AudioEvent{AudioEvent::Type::MIDI_CLOCK_OUT, offset, 1});
		}
		playbackHandler.scheduleMIDIClockOutTick();
	}
}

/* --------------------------------------------------------------------------
 * Drain sequencer events from the storage task (lock-free SPSC queue).
 * Called each sequencer cycle. One atomic load when empty — negligible cost.
 * -------------------------------------------------------------------------- */
static void handleSongReady(Song* newSong, void* requestingTask) {
	Song* old = currentSong;

	playbackHandler.doSongSwap();

	audioFileManager.thingFinishedLoading();

	// Hand old song to storage task for teardown via task notification.
	xTaskNotify((TaskHandle_t)storageTaskHandle, reinterpret_cast<uint32_t>(old), eSetValueWithOverwrite);

	// Wake the app task so it can finalize the UI.
	if (requestingTask) {
		xTaskNotifyGive((TaskHandle_t)requestingTask);
	}
}

static void drainSequencerEvents() {
	SequencerEvent ev;
	while (g_sequencerEventQueue.pop(ev)) {
		switch (ev.type) {
		case SequencerEventType::SONG_READY:
			handleSongReady(ev.songReady.song, ev.songReady.requestingTask);
			break;
		case SequencerEventType::TEARDOWN_DONE:
			// Old song destruction complete. Could notify UI here.
			break;
		case SequencerEventType::LOAD_ERROR:
			// TODO: Notify UI of load failure
			break;
		}
	}
}

extern volatile uint32_t allocMutexContentionCount;

static void sequencerTaskFunction(void* pvParameters) {
	(void)pvParameters;
	uint8_t graphicsCounter = 0;
	uint16_t diagnosticCounter = 0;

	for (;;) {
		/* Block until DMA half-buffer transfer completes (~2.9ms).
		 * The DMA ISR notifies both tasks. Audio (priority 7) runs first
		 * and renders using voice state from our *previous* pass. When
		 * audio finishes and blocks, we run and prepare voice state for
		 * the *next* buffer. One buffer of latency, but no risk of
		 * underrun from sequencer processing time. */
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		/* Post-process any clusters the ISR has finished reading since last cycle.
		 * This does format conversion, boundary stitching, and marks them loaded. */
		drainClusterCompletions();

		/* Check for events from the storage task (song ready, errors, etc.).
		 * Lock-free SPSC — one atomic load when empty, negligible cost. */
		drainSequencerEvents();

		/* Submit pending cluster reads from the loading queue to the ISR.
		 * The ISR processes them autonomously between sequencer cycles. */
		feedClusterReadsToISR();

		sequencerRoutine();

		/* Arp phase advancement now happens on the audio task inside
		 * Sound::render(). The sequencer sends ARP_TICK events for
		 * tick-synced arp timing via doTickForwardForArp(). */

		/* Prefetch sample clusters for upcoming notes and active voices.
		 * Runs AFTER tick processing (which may have started new notes)
		 * and AFTER feeding the ISR (so newly prefetched clusters get
		 * submitted on the next cycle). */
		prefetchUpcomingSampleClusters();

		/* Graphics update every ~5th wake-up (~14.5ms) */
		if (++graphicsCounter >= 5) {
			graphicsCounter = 0;
			graphicsUpdate();
		}

		/* Display audio CPU load as percentage every ~0.5 seconds during playback.
		 * At 33.33MHz, the 1.45ms half-buffer deadline = ~48,330 cycles.
		 * Display as percentage: (maxCycles / 48330) * 100.
		 * Over 100% means the audio engine missed its deadline. */
	}
}

/* --------------------------------------------------------------------------
 * Entry point: create tasks and start the FreeRTOS scheduler
 * -------------------------------------------------------------------------- */
extern "C" void startFreeRTOS(void (*schedulerEntry)(void)) {
	/* Initialize queues and async SD layer before creating tasks */
	voiceEventQueueInit();
	storageTaskInit();
	sdAsyncInit();

	/* App task at priority 3 — all non-audio tasks run cooperatively here */
	xTaskCreateStatic(appTaskFunction, "App", 8192, (void*)schedulerEntry,
	                  3, /* Same priority for all non-audio, no time-slicing */
	                  sAppTaskStack, &sAppTaskTCB);

	/* Storage task at priority 2 — song/preset load/save, XML parsing.
	 * Below app task so UI always stays responsive during loading. */
	storageTaskHandle =
	    xTaskCreateStatic(storageTaskFunction, "Storage", 16384, NULL, 2, /* Below app (3), never preempts UI */
	                      sStorageTaskStack, &sStorageTaskTCB);

	/* Sequencer task at priority 6 — tick advancement, events, MIDI/CV I/O, UI graphics.
	 * Also handles cluster loading: feeds the ISR from the loading queue and
	 * post-processes completed cluster reads (format conversion, boundary stitching). */
	sequencerTaskHandle =
	    xTaskCreateStatic(sequencerTaskFunction, "Sequencer", 4096, NULL, configMAX_PRIORITIES - 2, /* Priority 6 */
	                      sSequencerTaskStack, &sSequencerTaskTCB);

	/* Audio task at highest priority — pure DSP, preempts everything */
	audioTaskHandle =
	    xTaskCreateStatic(audioTaskFunction, "Audio", 4096, NULL, configMAX_PRIORITIES - 1, /* Priority 7 */
	                      sAudioTaskStack, &sAudioTaskTCB);

	/* Start the FreeRTOS scheduler. This never returns. */
	vTaskStartScheduler();
}

#endif /* USE_FREERTOS */
