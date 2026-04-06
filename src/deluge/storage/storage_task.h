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

#ifdef USE_FREERTOS

#include "definitions_cxx.hpp"
#include <etl/queue_spsc_atomic.h>

class Song;
class Instrument;

/* --------------------------------------------------------------------------
 * StorageCommand — messages from UI/App task to Storage task
 * -------------------------------------------------------------------------- */

enum class StorageCommandType : uint8_t {
	LOAD_SONG,
};

/// Maximum path length for embedded path strings in commands.
/// FAT32 limit is 256 chars including null terminator.
constexpr int32_t kStoragePathMaxLen = 256;

struct StorageCommand {
	StorageCommandType type;

	void* requestingTask; // TaskHandle_t of the task that sent this command (for completion notification)

	union {
		struct {
			uint32_t filePointerSclust;
			uint32_t filePointerObjsize;
			char dirPath[kStoragePathMaxLen];
			char songName[kStoragePathMaxLen];
			char filename[kStoragePathMaxLen]; // with extension, to detect .Json
			bool isPlaying;
		} loadSong;
	};
};

/* --------------------------------------------------------------------------
 * SequencerEvent — messages from Storage task to Sequencer task
 * -------------------------------------------------------------------------- */

enum class SequencerEventType : uint8_t {
	SONG_READY,
	TEARDOWN_DONE,
	LOAD_ERROR,
};

struct SequencerEvent {
	SequencerEventType type;

	union {
		struct {
			Song* song;
			void* requestingTask; // TaskHandle_t to notify when swap completes
		} songReady;

		struct {
			Error error;
		} loadError;
	};
};

/// Lock-free SPSC queue: Storage task produces, Sequencer task consumes.
/// Sequencer polls this each cycle (~2.9ms). One atomic load when empty.
inline etl::queue_spsc_atomic<SequencerEvent, 8> g_sequencerEventQueue;

/// Initialize the storage command queue. Must be called before creating tasks.
void storageTaskInit();

/// Enqueue a command for the storage task. Blocks until space is available.
bool storageCommandEnqueue(const StorageCommand& cmd);

/// Storage task entry point — called by FreeRTOS, never returns.
void storageTaskFunction(void* pvParameters);

/// Global handle for the storage task (set during startFreeRTOS).
extern void* storageTaskHandle; // TaskHandle_t, void* to avoid FreeRTOS header in this header

/// Cluster pipeline counters for deterministic drain.
/// When submitted == completed, no cluster reads are in-flight or pending post-processing.
extern volatile uint32_t clusterReadsSubmitted;
extern volatile uint32_t clusterReadsCompleted;

#endif // USE_FREERTOS

/// Diagnostic marker — set before each step, displayed by fault handler on crash.
/// Available in all builds (FreeRTOS and non-FreeRTOS).
extern "C" {
extern volatile const char* g_lastDiagnostic;
extern void setNumeric(const char* text);
}
/// Set both the 7-seg display and the fault-handler diagnostic marker.
inline void setDiagnostic(const char* tag) {
	g_lastDiagnostic = tag;
	setNumeric(tag);
}
