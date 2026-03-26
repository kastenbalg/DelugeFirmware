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

/// Voice lifecycle states for the FreeRTOS voice state machine.
///
/// Under FreeRTOS, only the audio task may transition voices between states.
/// The sequencer, app, and MIDI tasks enqueue events via the voice event queue.
/// The audio task processes those events and performs all state transitions.
///
/// State machine:
///
///   FREE ──noteOn──> ACTIVE ──noteOff──> RELEASING ──envelope done──> CLEANUP ──> FREE
///                      │                                                 ↑
///                      │──fast release / steal──> FAST_RELEASING ────────┘
///
/// All transitions are performed exclusively by the audio task.
/// Other tasks can only READ the state (atomically) — never write it.
enum class VoiceState : uint8_t {
	/// Slot is available for allocation. No Voice resources are held.
	FREE = 0,

	/// Voice is actively rendering audio (oscillators, envelopes, filters running).
	/// Entered when the audio task processes a NOTE_ON event from the queue.
	ACTIVE,

	/// Voice envelope is in normal release stage (note was released).
	/// Entered when the audio task processes a NOTE_OFF event.
	/// Transitions to CLEANUP when envelope reaches zero.
	RELEASING,

	/// Voice is being rapidly released (voice stealing or kill).
	/// Entered when the audio task needs to free a slot for a new voice.
	/// Uses a fast envelope release rate. Transitions to CLEANUP when done.
	FAST_RELEASING,

	/// Voice has finished sounding and is ready for resource cleanup.
	/// The audio task will call unassignStuff() and transition to FREE.
	/// This is a transient state — voices don't stay here across render cycles.
	CLEANUP,
};
