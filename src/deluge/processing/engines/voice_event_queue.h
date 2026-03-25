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

#include <cstdint>
#include <etl/queue_spsc_atomic.h>

/// Lightweight events from the sequencer task to the audio task.
///
/// Note-on/off events are NOT queued here — the sequencer calls Sound::noteOn()
/// directly and modifies voice state before signalling the audio task to render.
/// This queue carries only events that need sample-accurate offset within the
/// current render buffer (sidechain hits, metronome triggers).
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
/// 32 entries is generous — at most a few sidechain hits and one metronome
/// trigger per half-buffer period.
inline etl::queue_spsc_atomic<AudioEvent, 32> g_audioEventQueue;
