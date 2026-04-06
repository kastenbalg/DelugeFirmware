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

#include "storage/storage_task.h"
extern "C" {
#include "fatfs/ff.h"
}
#include "FreeRTOS.h"
#include "memory/memory_allocator_interface.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "queue.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/song_loader.h"
#include "task.h"

/* --------------------------------------------------------------------------
 * Storage command queue — UI/App task sends commands, Storage task receives.
 * FreeRTOS static queue, capacity 4. Storage blocks on receive when idle.
 * -------------------------------------------------------------------------- */

static constexpr uint32_t kStorageCommandQueueCapacity = 4;
static StaticQueue_t sStorageCommandQueueStorage;
static uint8_t sStorageCommandQueueBuffer[kStorageCommandQueueCapacity * sizeof(StorageCommand)];
static QueueHandle_t sStorageCommandQueue = nullptr;

/* Global handle — set during startFreeRTOS, used for task notifications. */
void* storageTaskHandle = nullptr;

void storageTaskInit() {
	sStorageCommandQueue = xQueueCreateStatic(kStorageCommandQueueCapacity, sizeof(StorageCommand),
	                                          sStorageCommandQueueBuffer, &sStorageCommandQueueStorage);
}

bool storageCommandEnqueue(const StorageCommand& cmd) {
	if (sStorageCommandQueue == nullptr) {
		return false;
	}
	return xQueueSend(sStorageCommandQueue, &cmd, portMAX_DELAY) == pdTRUE;
}

/* --------------------------------------------------------------------------
 * handleLoadSong — builds a Song in isolation, signals sequencer to swap.
 *
 * Flow:
 * 1. Parse XML/JSON, construct Song, enqueue sample cluster loads
 * 2. Wait for all clusters to be loaded (sequencer feeds ISR each cycle)
 * 3. Push SONG_READY to sequencer event queue
 * 4. Block until sequencer sends old song pointer via task notification
 * 5. Destroy old song, push TEARDOWN_DONE
 * -------------------------------------------------------------------------- */
static void handleLoadSong(const StorageCommand& cmd) {
	// 1. Build new Song from file
	FilePointer fp;
	fp.sclust = cmd.loadSong.filePointerSclust;
	fp.objsize = cmd.loadSong.filePointerObjsize;
	SongLoadResult result = loadSongFromFilePointer(&fp, cmd.loadSong.filename, cmd.loadSong.dirPath,
	                                                !cmd.loadSong.isPlaying, cmd.loadSong.songName);

	if (result.error != Error::NONE) {
		SequencerEvent ev{};
		ev.type = SequencerEventType::LOAD_ERROR;
		ev.loadError.error = result.error;
		g_sequencerEventQueue.push(ev);
		return;
	}

	// 2. Wait for all enqueued sample clusters to be loaded.
	while (audioFileManager.loadingQueueHasAnyLowestPriorityElements()) {
		vTaskDelay(pdMS_TO_TICKS(5));
	}

	// 3. Signal sequencer that the new Song is ready
	{
		SequencerEvent ev{};
		ev.type = SequencerEventType::SONG_READY;
		ev.songReady.song = result.song;
		ev.songReady.requestingTask = cmd.requestingTask;
		g_sequencerEventQueue.push(ev);
	}

	// 4. Block until sequencer sends old song pointer via task notification.
	uint32_t oldSongRaw = 0;
	xTaskNotifyWait(0, UINT32_MAX, &oldSongRaw, portMAX_DELAY);
	Song* oldSong = reinterpret_cast<Song*>(oldSongRaw);

	// 5. Destroy old song
	if (oldSong) {
		void* toDealloc = dynamic_cast<void*>(oldSong);
		oldSong->~Song();
		delugeDealloc(toDealloc);
	}

	// 6. Notify sequencer that teardown is complete
	{
		SequencerEvent ev{};
		ev.type = SequencerEventType::TEARDOWN_DONE;
		g_sequencerEventQueue.push(ev);
	}
}

/* --------------------------------------------------------------------------
 * Storage task function — blocks on command queue, dispatches handlers.
 * -------------------------------------------------------------------------- */

void storageTaskFunction(void* pvParameters) {
	(void)pvParameters;
	StorageCommand cmd;
	for (;;) {
		if (xQueueReceive(sStorageCommandQueue, &cmd, portMAX_DELAY) == pdTRUE) {
			switch (cmd.type) {
			case StorageCommandType::LOAD_SONG:
				handleLoadSong(cmd);
				break;
			default:
				break;
			}
		}
	}
}

#endif // USE_FREERTOS
