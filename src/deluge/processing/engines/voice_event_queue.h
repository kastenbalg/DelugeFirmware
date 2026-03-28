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
#include "task.h"
#endif

class Sound;
class InstrumentClip;
class ParamManager;
class Kit;
class Drum;
class ArpeggiatorBase;

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
	// Pre-arp events: sequencer/UI/MIDI send these, audio task runs arp + voice creation
	PRE_ARP_NOTE_ON,  // Pre-arp noteOn → audio task calls Sound::noteOn() with arp
	PRE_ARP_NOTE_OFF, // Pre-arp noteOff → audio task calls Sound::noteOff() with arp
	KIT_NOTE_ON,      // Kit drum noteOn → audio task calls Kit::noteOnPreKitArp()
	KIT_NOTE_OFF,     // Kit drum noteOff → audio task calls Kit::noteOffPreKitArp()
	ARP_TICK,         // Arp tick → audio task calls doTickForwardForArp()
	KIT_ARP_TICK,     // Kit arp tick → audio task calls Kit::doTickForwardForArp()

	// Legacy post-arp events (kept as safety net for any remaining direct callers)
	NOTE_ON,  // Post-arp: start a new voice directly
	NOTE_OFF, // Post-arp: release a voice (envelope release stage)
	LEGATO,   // Change note code on existing voice (mono mode)

	// Control events
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
		// PRE_ARP_NOTE_ON — pre-arp note, audio task runs arp + voice creation
		struct {
			InstrumentClip* clip;         // For ModelStack reconstruction
			ParamManager* paramManager;   // Exact paramManager at enqueue time
			ArpeggiatorBase* arpeggiator; // Sound's arpeggiator
			int32_t noteCode;             // Pre-arp note code
			int32_t velocity;
			int16_t mpeValues[kNumExpressionDimensions]; // X, Y, Z
			uint32_t sampleSyncLength;
			int32_t ticksLate;
			uint32_t samplesLate;
			int32_t fromMIDIChannel;
		} preArpNoteOn;

		// PRE_ARP_NOTE_OFF — pre-arp noteOff, audio task runs arp
		struct {
			InstrumentClip* clip;
			ParamManager* paramManager;
			ArpeggiatorBase* arpeggiator;
			int32_t noteCode; // Pre-arp note code
		} preArpNoteOff;

		// KIT_NOTE_ON — kit drum note, audio task runs kit arp + drum arp
		struct {
			Kit* kit;
			Drum* drum;
			InstrumentClip* clip;
			ParamManager* paramManager;
			uint8_t velocity;
			int16_t mpeValues[kNumExpressionDimensions];
			int32_t fromMIDIChannel;
			uint32_t sampleSyncLength;
			int32_t ticksLate;
			uint32_t samplesLate;
		} kitNoteOn;

		// KIT_NOTE_OFF — kit drum noteOff
		struct {
			Kit* kit;
			Drum* drum;
			InstrumentClip* clip;
			ParamManager* paramManager;
			int32_t velocity;
		} kitNoteOff;

		// ARP_TICK — arp tick for SoundInstrument
		struct {
			int32_t currentPos;
		} arpTick;

		// KIT_ARP_TICK — arp tick for Kit
		struct {
			Kit* kit;
			int32_t currentPos;
		} kitArpTick;

		// NOTE_ON (legacy post-arp)
		struct {
			InstrumentClip* clip;       // For ModelStack reconstruction
			ParamManager* paramManager; // Exact paramManager at enqueue time (NoteRow's for Kit drums)
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

		// KILL_SOUND (synchronous variant carries a task to notify on completion)
		struct {
			TaskHandle_t waitingTask; // non-null → notify this task when done
		} killSound;

		// KILL_ALL / PHASE_RECALC: no additional data needed
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

/* Diagnostic counters for monitoring queue health. */
extern volatile uint32_t voiceEventDropCount;
extern volatile uint32_t voiceEventEnqueueCount;

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

inline bool voiceEventNoteOn(Sound* sound, InstrumentClip* clip, ParamManager* paramManager, int32_t noteCodePreArp,
                             int32_t noteCodePostArp, int32_t velocity, const int16_t* mpeValues,
                             uint32_t sampleSyncLength, int32_t ticksLate, uint32_t samplesLate,
                             int32_t fromMIDIChannel) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::NOTE_ON;
	ev.sound = sound;
	ev.noteOn.clip = clip;
	ev.noteOn.paramManager = paramManager;
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

/* ---- Pre-arp event enqueue helpers ---- */

inline bool voiceEventPreArpNoteOn(Sound* sound, InstrumentClip* clip, ParamManager* paramManager,
                                   ArpeggiatorBase* arpeggiator, int32_t noteCode, const int16_t* mpeValues,
                                   uint32_t sampleSyncLength, int32_t ticksLate, uint32_t samplesLate, int32_t velocity,
                                   int32_t fromMIDIChannel) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::PRE_ARP_NOTE_ON;
	ev.sound = sound;
	ev.preArpNoteOn.clip = clip;
	ev.preArpNoteOn.paramManager = paramManager;
	ev.preArpNoteOn.arpeggiator = arpeggiator;
	ev.preArpNoteOn.noteCode = noteCode;
	ev.preArpNoteOn.velocity = velocity;
	if (mpeValues) {
		for (int32_t i = 0; i < kNumExpressionDimensions; i++) {
			ev.preArpNoteOn.mpeValues[i] = mpeValues[i];
		}
	}
	ev.preArpNoteOn.sampleSyncLength = sampleSyncLength;
	ev.preArpNoteOn.ticksLate = ticksLate;
	ev.preArpNoteOn.samplesLate = samplesLate;
	ev.preArpNoteOn.fromMIDIChannel = fromMIDIChannel;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventPreArpNoteOff(Sound* sound, InstrumentClip* clip, ParamManager* paramManager,
                                    ArpeggiatorBase* arpeggiator, int32_t noteCode) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::PRE_ARP_NOTE_OFF;
	ev.sound = sound;
	ev.preArpNoteOff.clip = clip;
	ev.preArpNoteOff.paramManager = paramManager;
	ev.preArpNoteOff.arpeggiator = arpeggiator;
	ev.preArpNoteOff.noteCode = noteCode;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventKitNoteOn(Kit* kit, Drum* drum, InstrumentClip* clip, ParamManager* paramManager,
                                uint8_t velocity, const int16_t* mpeValues, int32_t fromMIDIChannel,
                                uint32_t sampleSyncLength, int32_t ticksLate, uint32_t samplesLate) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::KIT_NOTE_ON;
	ev.sound = nullptr; // Kit events use kit pointer instead
	ev.kitNoteOn.kit = kit;
	ev.kitNoteOn.drum = drum;
	ev.kitNoteOn.clip = clip;
	ev.kitNoteOn.paramManager = paramManager;
	ev.kitNoteOn.velocity = velocity;
	if (mpeValues) {
		for (int32_t i = 0; i < kNumExpressionDimensions; i++) {
			ev.kitNoteOn.mpeValues[i] = mpeValues[i];
		}
	}
	ev.kitNoteOn.fromMIDIChannel = fromMIDIChannel;
	ev.kitNoteOn.sampleSyncLength = sampleSyncLength;
	ev.kitNoteOn.ticksLate = ticksLate;
	ev.kitNoteOn.samplesLate = samplesLate;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventKitNoteOff(Kit* kit, Drum* drum, InstrumentClip* clip, ParamManager* paramManager,
                                 int32_t velocity) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::KIT_NOTE_OFF;
	ev.sound = nullptr;
	ev.kitNoteOff.kit = kit;
	ev.kitNoteOff.drum = drum;
	ev.kitNoteOff.clip = clip;
	ev.kitNoteOff.paramManager = paramManager;
	ev.kitNoteOff.velocity = velocity;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventArpTick(Sound* sound, int32_t currentPos) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::ARP_TICK;
	ev.sound = sound;
	ev.arpTick.currentPos = currentPos;
	return voiceEventEnqueue(ev);
}

inline bool voiceEventKitArpTick(Kit* kit, int32_t currentPos) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::KIT_ARP_TICK;
	ev.sound = nullptr;
	ev.kitArpTick.kit = kit;
	ev.kitArpTick.currentPos = currentPos;
	return voiceEventEnqueue(ev);
}

/* ---- Control event enqueue helpers ---- */

inline bool voiceEventKillSound(Sound* sound, TaskHandle_t waitingTask = nullptr) {
	VoiceEvent ev{};
	ev.type = VoiceEventType::KILL_SOUND;
	ev.sound = sound;
	ev.killSound.waitingTask = waitingTask;
	return voiceEventEnqueue(ev);
}

/* Synchronous kill: enqueues KILL_SOUND and blocks until the audio task
 * has fully processed wontBeRenderedForAWhile(). Use from non-audio tasks
 * when the caller needs cleanup to complete before continuing (e.g. preset switch). */
inline void voiceEventKillSoundSync(Sound* sound) {
	TaskHandle_t me = xTaskGetCurrentTaskHandle();
	ulTaskNotifyTake(pdTRUE, 0); // clear any pending notification
	voiceEventKillSound(sound, me);
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // block until audio task signals
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
