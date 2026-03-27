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

/*
 * ISR-driven asynchronous SD card access layer for FreeRTOS.
 *
 * All SD card operations are funneled through a single ISR state machine
 * that drives the SDHI hardware asynchronously via DMA. The ISR runs at
 * interrupt priority, outside the FreeRTOS scheduler, so SD transfers
 * proceed concurrently with audio rendering and sequencer processing.
 *
 * Two request paths:
 * - Fast path (cluster reads): non-blocking, callback on completion
 * - Slow path (FatFS reads/writes): blocks calling task on semaphore
 *
 * The ISR interleaves fast and slow requests at the sector level with
 * a configurable ratio (SD_FAST_SLOW_RATIO fast sectors per 1 slow sector).
 * When one queue is empty, the other gets full bandwidth.
 *
 * This replaces sdCardMutex — the ISR is the sole owner of the SDHI
 * hardware. No task ever touches SDHI directly after initialization.
 */

#ifdef USE_FREERTOS

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interleaving ratio: N fast-path sectors per 1 slow-path sector.
 * Only applies when both queues have pending work. */
#define SD_FAST_SLOW_RATIO 2

/* Queue capacities */
#define SD_FAST_QUEUE_SIZE 16 /* Cluster read requests (each up to 64 sectors) */
#define SD_SLOW_QUEUE_SIZE 8  /* FatFS read/write requests (each 1-8 sectors) */

/* Request types */
typedef enum {
	SD_REQ_READ,         /* Read sectors (slow path — FatFS) */
	SD_REQ_WRITE,        /* Write sectors (slow path — FatFS) */
	SD_REQ_READ_CLUSTER, /* Read sectors (fast path — cluster loader) */
} SdReqType;

/* Completion callback for fast path (cluster reads).
 * Called from ISR context — must be brief (set a flag, give a semaphore). */
typedef void (*SdAsyncCallback)(int32_t result, void* userData);

/* SD request descriptor */
typedef struct {
	SdReqType type;
	uint32_t sector;      /* Starting LBA sector */
	uint8_t* buffer;      /* Source (write) or destination (read) buffer */
	uint32_t sectorCount; /* Total sectors in this request */

	/* For slow path: semaphore to give on completion */
	void* completionSem;         /* SemaphoreHandle_t */
	volatile int32_t* resultPtr; /* Where to store the result */

	/* For fast path: callback on completion */
	SdAsyncCallback callback;
	void* userData;
} SdRequest;

/* Initialize the async SD layer. Must be called before scheduler starts
 * but after sd_init has configured the SDHI hardware. */
void sdAsyncInit(void);

/* Start the ISR-driven processing. Call after scheduler starts.
 * Hooks into the existing SDHI interrupt handler. */
void sdAsyncStart(void);

/*
 * Fast path: enqueue a cluster read request.
 * Non-blocking. The callback is called from the SD worker context
 * when all sectors have been transferred.
 *
 * Returns true if enqueued, false if queue is full (request dropped).
 */
bool sdAsyncReadCluster(uint32_t sector, uint8_t* buffer, uint32_t sectorCount, SdAsyncCallback callback,
                        void* userData);

/*
 * Slow path: synchronous disk read for FatFS.
 * Enqueues the request and blocks the calling task until completion.
 * Returns 0 on success, nonzero on error.
 */
int32_t sdAsyncSyncRead(uint32_t sector, uint8_t* buffer, uint32_t sectorCount);

/*
 * Slow path: synchronous disk write for FatFS.
 * Enqueues the request and blocks the calling task until completion.
 * Returns 0 on success, nonzero on error.
 */
int32_t sdAsyncSyncWrite(uint32_t sector, const uint8_t* buffer, uint32_t sectorCount);

/* Check if the async layer is active (post-scheduler). Before the scheduler
 * starts, callers should use the old synchronous sd_read_sect/sd_write_sect. */
bool sdAsyncIsActive(void);
int sdAsyncGetState(void); /* Returns current SdAsyncState as int, for diagnostics */

#ifdef __cplusplus
}
#endif

#endif /* USE_FREERTOS */
