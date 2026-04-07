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

#ifdef USE_FREERTOS

#include "processing/engines/voice_event_queue.h"
#include "extern.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "model/voice/voice.h"
#include "modulation/arpeggiator.h"
#include "modulation/params/param_manager.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "task.h"

extern Song* currentSong;

/* Static storage for the FreeRTOS queue (no dynamic allocation). */
static StaticQueue_t sVoiceEventQueueStorage;
static uint8_t sVoiceEventQueueBuffer[kVoiceEventQueueCapacity * sizeof(VoiceEvent)];
static QueueHandle_t sVoiceEventQueue = nullptr;

/* Audio task handle — set by startFreeRTOS, used by isAudioTask(). */
extern TaskHandle_t audioTaskHandle;

bool isAudioTask() {
	return xTaskGetCurrentTaskHandle() == audioTaskHandle;
}

void voiceEventQueueInit() {
	sVoiceEventQueue = xQueueCreateStatic(kVoiceEventQueueCapacity, sizeof(VoiceEvent), sVoiceEventQueueBuffer,
	                                      &sVoiceEventQueueStorage);
}

/* Diagnostic counters — visible in debugger or via OLED display. */
volatile uint32_t voiceEventDropCount = 0;
volatile uint32_t voiceEventEnqueueCount = 0;

bool voiceEventEnqueue(const VoiceEvent& event) {
	if (sVoiceEventQueue == nullptr) {
		return false;
	}
	/* Non-blocking send. If the queue is full, the event is dropped.
	 * This is intentional — dropping a noteOn under extreme load is
	 * preferable to blocking the sequencer/UI and causing a cascade. */
	if (xQueueSend(sVoiceEventQueue, &event, 0) == pdTRUE) {
		voiceEventEnqueueCount++;
		return true;
	}
	voiceEventDropCount++;
	return false;
}

/* Process a single voice event. Called from voiceEventProcessAll().
 * All voice operations happen here, in the audio task context.
 *
 * TODO: Implement each event type. Currently stubs that will be filled
 * in as we modify each caller to use the queue instead of direct calls.
 */
/* Build a ModelStackWithSoundFlags from a Sound and InstrumentClip.
 * Used to reconstruct the ModelStack that noteOnPostArpeggiator and
 * noteOffPostArpeggiator expect, from the data stored in voice events.
 * paramManager overrides clip->paramManager when non-null — needed for
 * Kit NoteRows where the per-row paramManager differs from the clip's. */
static ModelStackWithSoundFlags* buildModelStack(char* memory, Sound* sound, InstrumentClip* clip,
                                                 ParamManager* paramManager = nullptr) {
	if (currentSong == nullptr || sound == nullptr) {
		return nullptr;
	}
	ModelStack* ms = (ModelStack*)memory;
	ms->song = currentSong;

	ParamManager* pm = (paramManager != nullptr) ? paramManager : (clip != nullptr) ? &clip->paramManager : nullptr;

	auto* ms3 = ms->addTimelineCounter(clip)->addOtherTwoThingsButNoNoteRow(sound, pm);
	return ms3->addSoundFlags();
}

/* Build a ModelStackWithThreeMainThings for pre-arp noteOn/noteOff.
 * These functions take ThreeMainThings, not SoundFlags. */
static ModelStackWithThreeMainThings* buildModelStack3(char* memory, Sound* sound, InstrumentClip* clip,
                                                       ParamManager* paramManager = nullptr) {
	if (currentSong == nullptr || sound == nullptr) {
		return nullptr;
	}
	ModelStack* ms = (ModelStack*)memory;
	ms->song = currentSong;

	ParamManager* pm = (paramManager != nullptr) ? paramManager : (clip != nullptr) ? &clip->paramManager : nullptr;

	return ms->addTimelineCounter(clip)->addOtherTwoThingsButNoNoteRow(sound, pm);
}

static void processVoiceEvent(const VoiceEvent& event) {
	switch (event.type) {

		/* ---- Pre-arp events: audio task runs arp + voice creation ---- */

	case VoiceEventType::PRE_ARP_NOTE_ON: {
		char modelStackMemory[256];
		ModelStackWithThreeMainThings* ms3 =
		    buildModelStack3(modelStackMemory, event.sound, event.preArpNoteOn.clip, event.preArpNoteOn.paramManager);
		if (ms3 != nullptr) {
			event.sound->noteOn(ms3, event.preArpNoteOn.arpeggiator, event.preArpNoteOn.noteCode,
			                    event.preArpNoteOn.mpeValues, event.preArpNoteOn.sampleSyncLength,
			                    event.preArpNoteOn.ticksLate, event.preArpNoteOn.samplesLate,
			                    event.preArpNoteOn.velocity, event.preArpNoteOn.fromMIDIChannel);
		}
		break;
	}

	case VoiceEventType::PRE_ARP_NOTE_OFF: {
		char modelStackMemory[256];
		ModelStackWithThreeMainThings* ms3 =
		    buildModelStack3(modelStackMemory, event.sound, event.preArpNoteOff.clip, event.preArpNoteOff.paramManager);
		if (ms3 != nullptr) {
			event.sound->noteOff(ms3, event.preArpNoteOff.arpeggiator, event.preArpNoteOff.noteCode);
		}
		break;
	}

	case VoiceEventType::KIT_NOTE_ON: {
		char modelStackMemory[256];
		Kit* kit = event.kitNoteOn.kit;
		Drum* drum = event.kitNoteOn.drum;
		if (kit != nullptr && drum != nullptr && currentSong != nullptr) {
			ModelStack* ms = (ModelStack*)modelStackMemory;
			ms->song = currentSong;
			/* ModelStack must use drum->toModControllable() (the SoundDrum as ModControllable)
			 * and the drum's paramManager — this gets passed through to drum->noteOn() → Sound::noteOn(). */
			auto* ms3 = ms->addTimelineCounter(event.kitNoteOn.clip)
			                ->addOtherTwoThingsButNoNoteRow(drum->toModControllable(), event.kitNoteOn.paramManager);
			kit->noteOnPreKitArp(ms3, drum, event.kitNoteOn.velocity, event.kitNoteOn.mpeValues,
			                     event.kitNoteOn.fromMIDIChannel, event.kitNoteOn.sampleSyncLength,
			                     event.kitNoteOn.ticksLate, event.kitNoteOn.samplesLate);
		}
		break;
	}

	case VoiceEventType::KIT_NOTE_OFF: {
		char modelStackMemory[256];
		Kit* kit = event.kitNoteOff.kit;
		Drum* drum = event.kitNoteOff.drum;
		if (kit != nullptr && drum != nullptr && currentSong != nullptr) {
			ModelStack* ms = (ModelStack*)modelStackMemory;
			ms->song = currentSong;
			auto* ms3 = ms->addTimelineCounter(event.kitNoteOff.clip)
			                ->addOtherTwoThingsButNoNoteRow(drum->toModControllable(), event.kitNoteOff.paramManager);
			kit->noteOffPreKitArp(ms3, drum, event.kitNoteOff.velocity);
		}
		break;
	}

	case VoiceEventType::ARP_TICK: {
		if (event.sound != nullptr && currentSong != nullptr) {
			char modelStackMemory[256];
			ModelStack* ms = (ModelStack*)modelStackMemory;
			ms->song = currentSong;
			static_cast<SoundInstrument*>(event.sound)->doTickForwardForArp(ms, event.arpTick.currentPos);
		}
		break;
	}

	case VoiceEventType::KIT_ARP_TICK: {
		Kit* kit = event.kitArpTick.kit;
		if (kit != nullptr && currentSong != nullptr) {
			char modelStackMemory[256];
			ModelStack* ms = (ModelStack*)modelStackMemory;
			ms->song = currentSong;
			kit->doTickForwardForArp(ms, event.kitArpTick.currentPos);
		}
		break;
	}

		/* ---- Legacy post-arp events (safety net for remaining direct callers) ---- */

	case VoiceEventType::NOTE_ON: {
		/* Reconstruct ModelStack and call noteOnPostArpeggiator.
		 * This runs in the audio task — isAudioTask() returns true,
		 * so noteOnPostArpeggiator executes directly instead of re-enqueueing. */
		char modelStackMemory[256]; /* ModelStack is small, stack-allocated */
		ModelStackWithSoundFlags* msf =
		    buildModelStack(modelStackMemory, event.sound, event.noteOn.clip, event.noteOn.paramManager);
		if (msf != nullptr) {
			event.sound->noteOnPostArpeggiator(msf, event.noteOn.noteCodePreArp, event.noteOn.noteCodePostArp,
			                                   event.noteOn.velocity, event.noteOn.mpeValues,
			                                   event.noteOn.sampleSyncLength, event.noteOn.ticksLate,
			                                   event.noteOn.samplesLate, event.noteOn.fromMIDIChannel);
		}
		break;
	}

	case VoiceEventType::NOTE_OFF: {
		char modelStackMemory[256];
		/* NOTE_OFF doesn't carry a clip pointer — use the Sound's activeClip */
		InstrumentClip* clip = nullptr;
		if (event.sound != nullptr) {
			clip = static_cast<InstrumentClip*>(static_cast<SoundInstrument*>(event.sound)->getActiveClip());
		}
		ModelStackWithSoundFlags* msf = buildModelStack(modelStackMemory, event.sound, clip);
		if (msf != nullptr) {
			event.sound->noteOffPostArpeggiator(msf, event.noteOff.noteCodePostArp);
		}
		break;
	}

	case VoiceEventType::LEGATO: {
		/* Legato is handled by noteOnPostArpeggiator — when polyphony mode is
		 * LEGATO, it finds the existing voice and calls changeNoteCode internally.
		 * We just route through NOTE_ON with the legato note codes. */
		if (event.sound != nullptr) {
			char modelStackMemory[256];
			InstrumentClip* clip = nullptr;
			clip = static_cast<InstrumentClip*>(static_cast<SoundInstrument*>(event.sound)->getActiveClip());
			ModelStackWithSoundFlags* msf = buildModelStack(modelStackMemory, event.sound, clip);
			if (msf != nullptr) {
				event.sound->noteOnPostArpeggiator(msf, event.legato.noteCodePreArp, event.legato.noteCodePostArp,
				                                   0, /* velocity — not used for legato transition */
				                                   event.legato.mpeValues, 0, 0, 0, event.legato.fromMIDIChannel);
			}
		}
		break;
	}

	case VoiceEventType::KILL_SOUND:
		if (event.sound != nullptr) {
			/* Full cleanup: kills voices, discards delay buffers,
			 * ends stutter, resets arp/sidechain, reassesses render skipping.
			 * Safe here because we're on the audio task. */
			event.sound->wontBeRenderedForAWhile();
		}
		/* If a task is waiting for synchronous completion, wake it. */
		if (event.killSound.waitingTask != nullptr) {
			xTaskNotifyGive(event.killSound.waitingTask);
		}
		break;

	case VoiceEventType::KILL_ALL:
		AudioEngine::killAllVoices();
		if (event.killAll.waitingTask != nullptr) {
			xTaskNotifyGive(event.killAll.waitingTask);
		}
		break;

	case VoiceEventType::EXPRESSION:
		/* TODO: Find the matching voice and call
		 * voice->expressionEventSmooth/Immediate. */
		break;

	case VoiceEventType::PARAM_CHANGE:
		/* TODO: Call sound->patchedParamPresetValueChanged. */
		break;

	case VoiceEventType::PHASE_RECALC:
		/* TODO: Call sound->recalculateAllVoicePhaseIncrements. */
		break;

	case VoiceEventType::PREVIEW_NOTE_ON: {
		SoundDrum* preview = AudioEngine::sampleForPreview;
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = setupModelStackWithThreeMainThingsButNoNoteRow(
		    modelStackMemory, currentSong, preview, NULL, AudioEngine::paramManagerForSamplePreview);
		preview->Sound::noteOn(modelStack, &preview->arpeggiator, kNoteForDrum, zeroMPEValues);
		AudioEngine::bypassCulling = true;
		break;
	}
	}
}

int32_t voiceEventProcessAll() {
	if (sVoiceEventQueue == nullptr) {
		return 0;
	}

	int32_t count = 0;
	VoiceEvent event;

	/* Drain all pending events. Non-blocking — returns immediately
	 * when the queue is empty. */
	while (xQueueReceive(sVoiceEventQueue, &event, 0) == pdTRUE) {
		processVoiceEvent(event);
		count++;
	}

	return count;
}

#endif /* USE_FREERTOS */
