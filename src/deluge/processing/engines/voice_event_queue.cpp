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
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"

/* Static storage for the FreeRTOS queue (no dynamic allocation). */
static StaticQueue_t sVoiceEventQueueStorage;
static uint8_t sVoiceEventQueueBuffer[kVoiceEventQueueCapacity * sizeof(VoiceEvent)];
static QueueHandle_t sVoiceEventQueue = nullptr;

void voiceEventQueueInit() {
	sVoiceEventQueue = xQueueCreateStatic(kVoiceEventQueueCapacity, sizeof(VoiceEvent), sVoiceEventQueueBuffer,
	                                      &sVoiceEventQueueStorage);
}

bool voiceEventEnqueue(const VoiceEvent& event) {
	if (sVoiceEventQueue == nullptr) {
		return false;
	}
	/* Non-blocking send. If the queue is full, the event is dropped.
	 * This is intentional — dropping a noteOn under extreme load is
	 * preferable to blocking the sequencer/UI and causing a cascade. */
	return xQueueSend(sVoiceEventQueue, &event, 0) == pdTRUE;
}

/* Process a single voice event. Called from voiceEventProcessAll().
 * All voice operations happen here, in the audio task context.
 *
 * TODO: Implement each event type. Currently stubs that will be filled
 * in as we modify each caller to use the queue instead of direct calls.
 */
static void processVoiceEvent(const VoiceEvent& event) {
	switch (event.type) {
	case VoiceEventType::NOTE_ON:
		/* TODO: Call Sound::noteOnPostArpeggiator from here.
		 * Requires reconstructing ModelStackWithSoundFlags from
		 * event.sound and event.noteOn.clip. */
		break;

	case VoiceEventType::NOTE_OFF:
		/* TODO: Call Sound::noteOffPostArpeggiator or find the
		 * matching voice and call voice->noteOff(). */
		break;

	case VoiceEventType::LEGATO:
		/* TODO: Find the matching voice and call voice->changeNoteCode(). */
		break;

	case VoiceEventType::KILL_SOUND:
		if (event.sound != nullptr) {
			event.sound->killAllVoices();
		}
		break;

	case VoiceEventType::KILL_ALL:
		AudioEngine::killAllVoices();
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
