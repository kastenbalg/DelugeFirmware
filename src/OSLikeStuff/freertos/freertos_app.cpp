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
 * - Audio task at FreeRTOS priority 7 (highest) — pure DSP, drains voice event queue
 * - Sequencer task at priority 6 — tick advancement, event scheduling, MIDI/CV I/O, UI graphics
 * - Cluster loader task at priority 5 — loads sample clusters from SD card
 * - App task at FreeRTOS priority 3 — runs cooperative scheduler for file ops
 * - IRQ handler bridges to existing GIC dispatch via vApplicationFPUSafeIRQHandler
 */

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "semphr.h"
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

/* Cluster loader task: loads sample clusters from SD card. 8KB stack. */
static StaticTask_t sClusterLoaderTCB;
static StackType_t sClusterLoaderStack[2048];

/* Sequencer task: tick advancement, event scheduling, MIDI/CV I/O, UI. 16KB stack. */
static StaticTask_t sSequencerTaskTCB;
static StackType_t sSequencerTaskStack[4096];

/* Global handles for task management */
TaskHandle_t clusterLoaderTaskHandle = nullptr;
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
 * Two DMA descriptors (A and B) each cover half the 256-sample TX buffer.
 * When each 128-sample half-buffer transfer completes (~2.9ms at 44.1kHz),
 * the DMA interrupt fires and notifies the audio task. The audio task wakes,
 * renders into the half that just finished playing, then sleeps until the
 * next interrupt. Multiple notifications are drained for overload recovery.
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

static void audioTaskFunction(void* pvParameters) {
	(void)pvParameters;

	/* Initialize the DMA interrupt now that the scheduler is running */
	ssiTxDmaInterruptInit();

	for (;;) {
		/* Block until DMA half-buffer transfer completes.
		 * The ISR fires every 128 samples (~2.9ms at 44.1kHz).
		 * The audio task is a pure DSP consumer: it drains the voice event
		 * queue, renders all active voices/effects, and writes to the DMA buffer.
		 * Tick logic and I/O are handled by the sequencer task. */
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		AudioEngine::routine();
	}
}

/* --------------------------------------------------------------------------
 * Cluster loader task: dedicated task for loading sample clusters from SD
 * -------------------------------------------------------------------------- */
#include "storage/audio/audio_file_manager.h"

static void clusterLoaderTaskFunction(void* pvParameters) {
	(void)pvParameters;
	audioFileManager.clusterLoaderMain(); /* Infinite loop — never returns */
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

/* Placeholder for sequencer tick logic — will be implemented in Step 4.
 * For now, this is a no-op so the task structure compiles and runs. */
static void sequencerRoutine() {
	/* TODO: Phase 6b Step 4 — move tick logic from audio engine here */
}

static void sequencerTaskFunction(void* pvParameters) {
	(void)pvParameters;
	uint8_t graphicsCounter = 0;

	for (;;) {
		/* Block until DMA half-buffer transfer completes (~2.9ms).
		 * The DMA ISR notifies both tasks. Audio (priority 7) runs first
		 * and renders using voice state from our *previous* pass. When
		 * audio finishes and blocks, we run and prepare voice state for
		 * the *next* buffer. One buffer of latency, but no risk of
		 * underrun from sequencer processing time. */
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		sequencerRoutine();

		/* Graphics update every ~5th wake-up (~14.5ms) */
		if (++graphicsCounter >= 5) {
			graphicsCounter = 0;
			graphicsUpdate();
		}
	}
}

/* --------------------------------------------------------------------------
 * Entry point: create tasks and start the FreeRTOS scheduler
 * -------------------------------------------------------------------------- */
extern "C" void startFreeRTOS(void (*schedulerEntry)(void)) {
	/* App task at priority 3 — all non-audio tasks run cooperatively here */
	xTaskCreateStatic(appTaskFunction, "App", 8192, (void*)schedulerEntry,
	                  3, /* Same priority for all non-audio, no time-slicing */
	                  sAppTaskStack, &sAppTaskTCB);

	/* Cluster loader task at priority 5 — above app, below audio */
	clusterLoaderTaskHandle = xTaskCreateStatic(clusterLoaderTaskFunction, "ClusterLoader", 2048, NULL, 5,
	                                            sClusterLoaderStack, &sClusterLoaderTCB);

	/* Sequencer task at priority 6 — tick advancement, events, MIDI/CV I/O, UI graphics */
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
