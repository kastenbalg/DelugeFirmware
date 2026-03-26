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

#pragma once

#include "definitions_cxx.hpp"
#include <cstdint>

#ifdef USE_FREERTOS
#include "FreeRTOS.h"
#include "queue.h"
#endif

class Sound;
class InstrumentClip;

/*
 * Voice Event Queue — the sole interface for requesting voice state changes.
 *
 * Under FreeRTOS, voices are owned exclusively by the audio task. All other
 * tasks (sequencer, app/UI, MIDI) must enqueue events here rather than
 * modifying voice state directly. The audio task dequeues and processes
 * events at the start of each render cycle.
 *
 * This eliminates race conditions, double-frees, and the need for mutexes
 * on the voice access path.
 *
 * Multiple producers (sequencer, app, MIDI handlers) — one consumer (audio task).
 * Uses a FreeRTOS queue for thread-safe MPSC operation.
 */

/* ---- Audio events (sidechain, metronome) ---- */

/* These lightweight events need sample-accurate offset within the render buffer.
 * Kept as a separate SPSC path for minimal latency. */
#include <etl/queue_spsc_atomic.h>

struct AudioEvent {
	enum class Type : uint8_t {
		SIDECHAIN_HIT,
		METRONOME_TRIGGER,
	};

	Type type;
	uint8_t sampleOffset; ///< Sample position within the half-buffer (0-127)

	/// Payload — interpretation depends on type.
	/// SIDECHAIN_HIT: magnitude of the hit
	/// METRONOME_TRIGGER: phase increment (different for beat vs off-beat)
	int32_t value;
};

/// SPSC queue: sequencer task produces, audio task consumes.
inline etl::queue_spsc_atomic<AudioEvent, 32> g_audioEventQueue;

/* ---- Voice events (noteOn, noteOff, expression, etc.) ---- */

enum class VoiceEventType : uint8_t {
	NOTE_ON,      // Start a new voice
	NOTE_OFF,     // Release a voice (envelope release stage)
	LEGATO,       // Change note code on existing voice (mono mode)
	KILL_SOUND,   // Kill all voices for a specific Sound
	KILL_ALL,     // Kill all voices globally
	EXPRESSION,   // MPE expression event (X/Y/Z per voice)
	PARAM_CHANGE, // Patched parameter value changed
	PHASE_RECALC, // Recalculate phase increments (pitch change)
};

struct VoiceEvent {
	VoiceEventType type;
	Sound* sound; // Target sound (nullptr for KILL_ALL)

	union {
		// NOTE_ON
		struct {
			InstrumentClip* clip; // For ModelStack reconstruction
			int32_t noteCodePreArp;
			int32_t noteCodePostArp;
			int32_t velocity;
			int16_t mpeValues[kNumExpressionDimensions]; // X, Y, Z
			uint32_t sampleSyncLength;
			int32_t ticksLate;
			uint32_t samplesLate;
			int32_t fromMIDIChannel;
		} noteOn;

		// NOTE_OFF
		struct {
			int32_t noteCodePostArp;
			int32_t fromMIDIChannel;
			bool allowReleaseStage;
		} noteOff;

		// LEGATO (change note on existing voice in mono mode)
		struct {
			int32_t noteCodePreArp;
			int32_t noteCodePostArp;
			int32_t fromMIDIChannel;
			int16_t mpeValues[kNumExpressionDimensions];
		} legato;

		// EXPRESSION (MPE per-voice expression)
		struct {
			int32_t noteCodePostArp; // Identifies which voice
			int32_t fromMIDIChannel;
			int32_t value;
			uint8_t dimension; // 0=X, 1=Y, 2=Z (aftertouch)
			bool smooth;       // true = smooth interpolation, false = immediate
		} expression;

		// PARAM_CHANGE
		struct {
			uint8_t paramId;
			int32_t value;
		} paramChange;

		// KILL_SOUND / KILL_ALL / PHASE_RECALC: no additional data needed
	};
};

/* Queue capacity. Must handle bursts from multiple arp clips firing
 * simultaneously (4 clips × 8-note chords = 64 noteOn+noteOff), plus
 * MPE expression streams (8 voices × 3 dimensions = 24), plus UI
 * parameter changes. The audio task drains the entire queue every
 * render cycle (~1.45ms / 690Hz), so bursts are absorbed quickly.
 * 256 events at ~48 bytes each = ~12KB. */
static constexpr size_t kVoiceEventQueueCapacity = 256;

#ifdef USE_FREERTOS

/* Initialize the voice event queue. Must be called before the scheduler starts. */
void voiceEventQueueInit();

/* Returns true if the calling task is the audio task (the voice owner).
 * Used by noteOnPostArpeggiator/noteOffPostArpeggiator to decide whether
 * to execute directly (in audio task) or enqueue (from any other task). */
bool isAudioTask();

/* Enqueue a voice event. Called from any task (sequencer, app, MIDI handler).
 * Returns true if the event was enqueued, false if the queue is full.
 * Non-blocking — if the queue is full, the event is dropped. */
bool voiceEventEnqueue(const VoiceEvent& event);

/* Dequeue and process all pending voice events. Called from the audio task
 * at the start of each render cycle. Returns the number of events processed. */
int32_t voiceEventProcessAll();

/* ---- Convenience functions for common event types ---- */

inline bool voiceEventNoteOn(Sound* sound, InstrumentClip* clip, int32_t noteCodePreArp, int32_t noteCodePostArp,
                             int32_t velocity, const int16_t* mpeValues, uint32_t sampleSyncLength, int32_t ticksLate,
                             uint32_t samplesLate, int32_t fromMIDIChannel) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::NOTE_ON;
	ev.sound = sound;
	ev.noteOn.clip = clip;
	ev.noteOn.noteCodePreArp = noteCodePreArp;
	ev.noteOn.noteCodePostArp = noteCodePostArp;
	ev.noteOn.velocity = velocity;
	if (mpeValues) {
		for (int32_t i = 0; i < kNumExpressionDimensions; i++) {
			ev.noteOn.mpeValues[i] = mpeValues[i];
		}
	}
	ev.noteOn.sampleSyncLength = sampleSyncLength;
	ev.noteOn.ticksLate = ticksLate;
	ev.noteOn.samplesLate = samplesLate;
	ev.noteOn.fromMIDIChannel = fromMIDIChannel;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventNoteOff(Sound* sound, int32_t noteCodePostArp, int32_t fromMIDIChannel,
                              bool allowReleaseStage = true) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::NOTE_OFF;
	ev.sound = sound;
	ev.noteOff.noteCodePostArp = noteCodePostArp;
	ev.noteOff.fromMIDIChannel = fromMIDIChannel;
	ev.noteOff.allowReleaseStage = allowReleaseStage;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventKillSound(Sound* sound) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::KILL_SOUND;
	ev.sound = sound;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventKillAll() {
	VoiceEvent ev{};
	ev.type = VoiceEventType::KILL_ALL;
	ev.sound = nullptr;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventExpression(Sound* sound, int32_t noteCodePostArp, int32_t fromMIDIChannel, uint8_t dimension,
                                 int32_t value, bool smooth) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::EXPRESSION;
	ev.sound = sound;
	ev.expression.noteCodePostArp = noteCodePostArp;
	ev.expression.fromMIDIChannel = fromMIDIChannel;
	ev.expression.dimension = dimension;
	ev.expression.value = value;
	ev.expression.smooth = smooth;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventLegato(Sound* sound, int32_t noteCodePreArp, int32_t noteCodePostArp, int32_t fromMIDIChannel,
                             const int16_t* mpeValues) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::LEGATO;
	ev.sound = sound;
	ev.legato.noteCodePreArp = noteCodePreArp;
	ev.legato.noteCodePostArp = noteCodePostArp;
	ev.legato.fromMIDIChannel = fromMIDIChannel;
	if (mpeValues) {
		for (int32_t i = 0; i < kNumExpressionDimensions; i++) {
			ev.legato.mpeValues[i] = mpeValues[i];
		}
	}
	return voiceEventEnqueue(ev);
}

inline bool voiceEventPhaseRecalc(Sound* sound) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::PHASE_RECALC;
	ev.sound = sound;
	return voiceEventEnqueue(ev);
}

#endif /* USE_FREERTOS */
