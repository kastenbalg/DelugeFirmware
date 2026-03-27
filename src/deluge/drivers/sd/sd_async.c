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

#include "sd_async.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "RZA1/sdhi/inc/sdif.h"
#include "RZA1/sdhi/src/sd/inc/access/sd.h"
#include "RZA1/system/iodefine.h"

/* ---- Cache maintenance and data memory barrier ---- */
#include "RZA1/compiler/asm/inc/asm.h"

/* ---- SDHI driver internals ---- */
/* _sd_get_hndls is a macro defined in sd.h: #define _sd_get_hndls(a) SDHandle[a]
 * The extern declarations below use the prototypes from sd.h (included above).
 * Functions that sd.h already declares don't need re-declaration. */

/* DMA functions — not declared in sd.h */
extern int sddev_init_dma(int sd_port, unsigned long buff, unsigned long reg, long cnt, int dir);
extern int sddev_disable_dma(int sd_port);

/* Clock divider — declared in sdif.h but we need it here */
extern unsigned int sddev_get_clockdiv(int sd_port, int clock);

/* Original synchronous SD functions — used for diagnostic fallback */
extern int sd_read_sect(int sd_port, unsigned char* buff, unsigned long psn, long cnt);
extern int sd_write_sect(int sd_port, unsigned char const* buff, unsigned long psn, long cnt, int writemode);

/* SD port config — defined in RZA1/cpu_specific.h */
#include "RZA1/cpu_specific.h"

/* Forward declarations for functions used before definition */
static void handleAllEnd(void);
static void handleSectorDone(void);

/* ========================================================================
 * Simple lock-free SPSC ring buffers for fast and slow queues.
 * Single producer per queue (tasks with interrupts disabled during enqueue).
 * Single consumer (the ISR state machine).
 * ======================================================================== */

typedef struct {
	SdRequest entries[SD_FAST_QUEUE_SIZE];
	volatile uint32_t head; /* Written by producer (task) */
	volatile uint32_t tail; /* Written by consumer (ISR) */
} SdFastQueue;

typedef struct {
	SdRequest entries[SD_SLOW_QUEUE_SIZE];
	volatile uint32_t head;
	volatile uint32_t tail;
} SdSlowQueue;

static SdFastQueue sFastQueue;
static SdSlowQueue sSlowQueue;

static inline bool fastQueueEmpty(void) {
	return sFastQueue.head == sFastQueue.tail;
}

static inline bool slowQueueEmpty(void) {
	return sSlowQueue.head == sSlowQueue.tail;
}

static inline bool fastQueueFull(void) {
	return ((sFastQueue.head + 1) % SD_FAST_QUEUE_SIZE) == sFastQueue.tail;
}

static inline bool slowQueueFull(void) {
	return ((sSlowQueue.head + 1) % SD_SLOW_QUEUE_SIZE) == sSlowQueue.tail;
}

static bool fastQueueEnqueue(const SdRequest* req) {
	if (fastQueueFull()) {
		return false;
	}
	sFastQueue.entries[sFastQueue.head] = *req;
	__asm__ volatile("dmb" ::: "memory"); /* Ensure the entry is written before advancing head */
	sFastQueue.head = (sFastQueue.head + 1) % SD_FAST_QUEUE_SIZE;
	return true;
}

static bool slowQueueEnqueue(const SdRequest* req) {
	if (slowQueueFull()) {
		return false;
	}
	sSlowQueue.entries[sSlowQueue.head] = *req;
	__asm__ volatile("dmb" ::: "memory");
	sSlowQueue.head = (sSlowQueue.head + 1) % SD_SLOW_QUEUE_SIZE;
	return true;
}

static SdRequest* fastQueuePeek(void) {
	if (fastQueueEmpty()) {
		return NULL;
	}
	return &sFastQueue.entries[sFastQueue.tail];
}

static SdRequest* slowQueuePeek(void) {
	if (slowQueueEmpty()) {
		return NULL;
	}
	return &sSlowQueue.entries[sSlowQueue.tail];
}

static void fastQueueDequeue(void) {
	sFastQueue.tail = (sFastQueue.tail + 1) % SD_FAST_QUEUE_SIZE;
}

static void slowQueueDequeue(void) {
	sSlowQueue.tail = (sSlowQueue.tail + 1) % SD_SLOW_QUEUE_SIZE;
}

/* ========================================================================
 * ISR State Machine
 *
 * Drives the SDHI hardware through single-sector read/write operations.
 * Each ISR invocation advances one state. Between ISR calls, the SDHI
 * DMA runs autonomously.
 *
 * States:
 *   IDLE         — No active transfer. Dequeue next request.
 *   CMD_SENT     — CMD17/CMD24 issued, waiting for response interrupt.
 *   DMA_RUNNING  — DMA transfer in progress, waiting for completion.
 *   ALL_END_WAIT — Waiting for "All end" interrupt after DMA.
 *   WRITE_BUSY   — After write, waiting for card to finish programming.
 *   SECTOR_DONE  — Sector complete, decide next action (interleave).
 *   ERROR        — Error occurred, clean up and notify requester.
 * ======================================================================== */

typedef enum {
	SD_STATE_IDLE,
	SD_STATE_CMD_SENT,
	SD_STATE_DMA_RUNNING,
	SD_STATE_ALL_END_WAIT,
	SD_STATE_SECTOR_DONE,
	SD_STATE_ERROR,
} SdAsyncState;

/* Tracks the current transfer state */
static struct {
	SdAsyncState state;
	SdRequest* currentReq;       /* Points into the queue entry */
	bool currentIsFast;          /* Which queue the current request came from */
	uint32_t currentSector;      /* Next sector to transfer */
	uint8_t* currentBuffer;      /* Next buffer position */
	uint32_t sectorsRemaining;   /* Sectors left in current request */
	uint8_t fastCounter;         /* Fast-path sectors done in current round */
	SDHNDL* hndl;                /* SDHI hardware handle */
	int dma64;                   /* DMA 64-byte transfer mode flag */
	bool active;                 /* True after sdAsyncStart() */
	unsigned short savedClkCtrl; /* Clock register value saved during init */
} sState;

/* ---- Helper: compute the SD card address from sector number ---- */
static inline uint32_t sectorToAddr(uint32_t sector) {
	/* SDHC/SDXC: sector addressing. SD v1: byte addressing. */
	return (sState.hndl->csd_structure == 0x01) ? sector : (sector * 512);
}

/* ---- Start a single-sector read: send CMD17 ---- */
static void startSectorRead(void) {
	SDHNDL* hndl = sState.hndl;
	uint32_t addr = sectorToAddr(sState.currentSector);

	/* Match the exact sequence from the original _sd_single_read + _sd_send_mcmd.
	 * Order matters — the SDHI hardware has implicit assumptions about
	 * register write sequence. */

	/* Clear software interrupt tracking */
	hndl->int_info1 = 0;
	hndl->int_info2 = 0;

	/* Set block size and disable auto-CMD12 (same as sd_read_sect) */
	sd_outp(hndl, SD_SIZE, 512);
	sd_outp(hndl, SD_STOP, 0x0000);

	/* Step 1: Enable response and ILA interrupts (same as _sd_single_read line 497) */
	_sd_set_int_mask(hndl, SD_INFO1_MASK_RESP, SD_INFO2_MASK_ILA);

	/* Step 2: DMA mode setup (same as _sd_single_read lines 499-509) */
	if (hndl->trans_mode & SD_MODE_DMA_64) {
		sState.dma64 = SD_MODE_DMA_64;
		sd_outp(hndl, EXT_SWAP, 0x0100);
	}
	else {
		sState.dma64 = SD_MODE_DMA;
	}
	sd_outp(hndl, CC_EXT_MODE, 2); /* enable DMA */

	/* Step 3: Send CMD17 (same sequence as _sd_send_mcmd lines 270-283) */
	_sd_set_arg(hndl, (unsigned short)(addr >> 16), (unsigned short)(addr & 0xFFFF));

	/* Wait for clock divider ready */
	{
		int i;
		for (i = 0; i < 10000; i++) {
			if (sd_inp(hndl, SD_INFO2) & SD_INFO2_MASK_SCLKDIVEN) {
				break;
			}
		}
		if (i == 10000) {
			/* Clock divider not ready — card/hardware issue */
			hndl->error = SD_ERR_CBSY_ERROR;
			sState.state = SD_STATE_ERROR;
			return;
		}
	}

	/* Issue CMD17 */
	sd_outp(hndl, SD_CMD, CMD17);

	sState.state = SD_STATE_CMD_SENT;
}

/* ---- Start a single-sector write: send CMD24 ---- */
static void startSectorWrite(void) {
	SDHNDL* hndl = sState.hndl;
	uint32_t addr = sectorToAddr(sState.currentSector);

	hndl->int_info1 = 0;
	hndl->int_info2 = 0;

	sd_outp(hndl, SD_SIZE, 512);
	sd_outp(hndl, SD_STOP, 0x0000);

	if (hndl->trans_mode & SD_MODE_DMA_64) {
		sState.dma64 = SD_MODE_DMA_64;
		sd_outp(hndl, EXT_SWAP, 0x0100);
	}
	else {
		sState.dma64 = SD_MODE_DMA;
	}

	/* Enable DMA */
	sd_outp(hndl, CC_EXT_MODE, (unsigned short)(sd_inp(hndl, CC_EXT_MODE) | CC_EXT_MODE_DMASDRW));

	/* Enable response interrupt */
	_sd_set_int_mask(hndl, SD_INFO1_MASK_RESP, SD_INFO2_MASK_ILA);

	_sd_set_arg(hndl, (unsigned short)(addr >> 16), (unsigned short)(addr & 0xFFFF));

	/* Issue CMD24 (WRITE_SINGLE_BLOCK) */
	sd_outp(hndl, SD_CMD, CMD24);

	sState.state = SD_STATE_CMD_SENT;
}

/* ---- Handle command response received ---- */
static void handleCmdResponse(void) {
	SDHNDL* hndl = sState.hndl;

	/* Clear response/ILA masks */
	_sd_clear_int_mask(hndl, SD_INFO1_MASK_RESP, SD_INFO2_MASK_ILA);

	/* Check for errors */
	if (hndl->int_info2 & SD_INFO2_MASK_ERR) {
		_sd_check_info2_err(hndl);
		sState.state = SD_STATE_ERROR;
		return;
	}

	/* Check response received */
	if (!(hndl->int_info1 & SD_INFO1_MASK_RESP)) {
		hndl->error = SD_ERR_NO_RESP_ERROR;
		sState.state = SD_STATE_ERROR;
		return;
	}

	/* Clear response info */
	_sd_clear_info(hndl, SD_INFO1_MASK_RESP, SD_INFO2_MASK_ERR);
	hndl->int_info1 = 0;
	hndl->int_info2 = 0;

	/* Disable card detect interrupt for FIFO access */
	_sd_clear_int_mask(hndl, SD_INFO1_MASK_DET_CD, 0);

	/* Enable "All end" and error interrupts */
	_sd_set_int_mask(hndl, SD_INFO1_MASK_DATA_TRNS, SD_INFO2_MASK_ERR);

	/* Invalidate cache for read buffer (required before and after DMA) */
	bool isRead = (sState.currentReq->type != SD_REQ_WRITE);
	if (isRead) {
		v7_dma_inv_range((uintptr_t)sState.currentBuffer, (intptr_t)(sState.currentBuffer + 512));
	}

	/* Initialize DMAC */
	unsigned long regAddr = hndl->reg_base;
	if (sState.dma64 != SD_MODE_DMA_64) {
		regAddr += SD_BUF0;
	}

	int dir = isRead ? SD_TRANS_READ : SD_TRANS_WRITE;
	if (sddev_init_dma(hndl->sd_port, (unsigned long)sState.currentBuffer, regAddr, 512, dir) != SD_OK) {
		hndl->error = SD_ERR_CPU_IF;
		sState.state = SD_STATE_ERROR;
		return;
	}

	sState.state = SD_STATE_DMA_RUNNING;
	/* Now the DMAC runs autonomously. The next ISR fires when DMA completes. */
}

/* ---- Handle DMA completion ---- */
static void handleDmaComplete(void) {
	SDHNDL* hndl = sState.hndl;

	/* Disable DMA */
	sddev_disable_dma(hndl->sd_port);
	sd_outp(hndl, CC_EXT_MODE, (unsigned short)(sd_inp(hndl, CC_EXT_MODE) & ~CC_EXT_MODE_DMASDRW));

	/* Restore card detect interrupt */
	_sd_set_int_mask(hndl, SD_INFO1_MASK_DET_CD, 0);

	/* Wait for "All end" — this may have already fired with the DMA interrupt.
	 * Check if it's set. If not, we wait for the next ISR call. */
	if (hndl->int_info1 & SD_INFO1_MASK_DATA_TRNS) {
		/* Already done — proceed immediately */
		handleAllEnd();
	}
	else {
		/* Wait for the next ISR with DATA_TRNS set */
		sState.state = SD_STATE_ALL_END_WAIT;
	}
}

/* ---- Handle "All end" interrupt ---- */
static void handleAllEnd(void) {
	SDHNDL* hndl = sState.hndl;

	/* Check errors */
	if (hndl->int_info2 & SD_INFO2_MASK_ERR) {
		_sd_check_info2_err(hndl);
		sState.state = SD_STATE_ERROR;
		return;
	}

	/* Invalidate cache after read DMA */
	bool isRead = (sState.currentReq->type != SD_REQ_WRITE);
	if (isRead) {
		v7_dma_inv_range((uintptr_t)sState.currentBuffer, (intptr_t)(sState.currentBuffer + 512));
	}

	/* Clear All end bit and disable data transfer interrupts */
	_sd_clear_info(hndl, SD_INFO1_MASK_DATA_TRNS, 0x0000);
	_sd_clear_int_mask(hndl, SD_INFO1_MASK_DATA_TRNS, isRead ? SD_INFO2_MASK_BRE : SD_INFO2_MASK_BWE);

	/* Clear EXT_SWAP */
	sd_outp(hndl, EXT_SWAP, 0x0000);

	/* Reset interrupt info for next operation */
	hndl->int_info1 = 0;
	hndl->int_info2 = 0;

	sState.state = SD_STATE_SECTOR_DONE;
}

/* ---- Complete a request: notify the requester ---- */
static void completeRequest(SdRequest* req, int32_t result) {
	if (req->type == SD_REQ_READ_CLUSTER) {
		/* Fast path: call the callback */
		if (req->callback != NULL) {
			req->callback(result, req->userData);
		}
	}
	else {
		/* Slow path: store result and give semaphore */
		if (req->resultPtr != NULL) {
			*req->resultPtr = result;
		}
		if (req->completionSem != NULL) {
			BaseType_t xHigherPriorityTaskWoken = pdFALSE;
			xSemaphoreGiveFromISR((SemaphoreHandle_t)req->completionSem, &xHigherPriorityTaskWoken);
			/* Note: we don't call portYIELD_FROM_ISR here because
			 * the SDHI ISR handler in sd_dev_low.c does it. */
		}
	}
}

/* ---- Pick the next request based on interleaving ratio ---- */
static SdRequest* pickNextRequest(bool* isFast) {
	bool hasFast = !fastQueueEmpty();
	bool hasSlow = !slowQueueEmpty();

	if (!hasFast && !hasSlow) {
		return NULL;
	}

	if (hasFast && !hasSlow) {
		*isFast = true;
		sState.fastCounter = 0;
		return fastQueuePeek();
	}

	if (!hasFast && hasSlow) {
		*isFast = false;
		sState.fastCounter = 0;
		return slowQueuePeek();
	}

	/* Both queues have work — apply ratio */
	if (sState.fastCounter < SD_FAST_SLOW_RATIO) {
		*isFast = true;
		return fastQueuePeek();
	}
	else {
		*isFast = false;
		sState.fastCounter = 0;
		return slowQueuePeek();
	}
}

/* ---- Handle sector done: advance or switch requests ---- */
static void handleSectorDone(void) {
	/* Advance buffer and sector */
	sState.currentBuffer += 512;
	sState.currentSector++;
	sState.sectorsRemaining--;

	if (sState.sectorsRemaining > 0) {
		/* More sectors in current request. But check if we should
		 * interleave — only if we're on the fast path and have done
		 * enough fast sectors, AND the slow queue has work. */
		if (sState.currentIsFast) {
			sState.fastCounter++;
			if (sState.fastCounter >= SD_FAST_SLOW_RATIO && !slowQueueEmpty()) {
				/* Pause current fast request, switch to slow.
				 * We'll resume the fast request later. But we can't
				 * pause mid-request easily with the ring buffer...
				 *
				 * Instead: the interleaving happens at the sector level
				 * within a request. After SD_FAST_SLOW_RATIO fast sectors
				 * of the current request, do 1 slow sector, then continue
				 * the fast request. The slow sector comes from a different
				 * request (peeked, not dequeued until fully done). */

				/* Save fast request state — it stays at the head of the
				 * fast queue with updated sector/buffer/count. We update
				 * the queue entry in place. */
				SdRequest* fastReq = sState.currentReq;
				fastReq->sector = sState.currentSector;
				fastReq->buffer = sState.currentBuffer;
				fastReq->sectorCount = sState.sectorsRemaining;

				/* Switch to one slow sector */
				SdRequest* slowReq = slowQueuePeek();
				if (slowReq != NULL) {
					sState.currentReq = slowReq;
					sState.currentIsFast = false;
					sState.currentSector = slowReq->sector;
					sState.currentBuffer = slowReq->buffer;
					sState.sectorsRemaining = 1; /* Just one slow sector */
					sState.fastCounter = 0;

					/* Start the slow sector */
					if (slowReq->type == SD_REQ_WRITE) {
						startSectorWrite();
					}
					else {
						startSectorRead();
					}
					return;
				}
			}
		}
		else {
			/* We just did a slow sector (interleaved). Check if the slow
			 * request has more sectors. */
			SdRequest* slowReq = slowQueuePeek();
			if (slowReq != NULL) {
				/* Advance the slow request's state in the queue entry */
				slowReq->sector++;
				slowReq->buffer += 512;
				slowReq->sectorCount--;

				if (slowReq->sectorCount == 0) {
					/* Slow request fully done */
					completeRequest(slowReq, 0);
					slowQueueDequeue();
				}
			}

			/* Switch back to fast path if there's a pending fast request */
			SdRequest* fastReq = fastQueuePeek();
			if (fastReq != NULL) {
				sState.currentReq = fastReq;
				sState.currentIsFast = true;
				sState.currentSector = fastReq->sector;
				sState.currentBuffer = fastReq->buffer;
				sState.sectorsRemaining = fastReq->sectorCount;

				if (fastReq->type == SD_REQ_WRITE) {
					startSectorWrite();
				}
				else {
					startSectorRead();
				}
				return;
			}

			/* No fast request — continue with more slow sectors if any */
			slowReq = slowQueuePeek();
			if (slowReq != NULL && slowReq->sectorCount > 0) {
				sState.currentReq = slowReq;
				sState.currentSector = slowReq->sector;
				sState.currentBuffer = slowReq->buffer;
				sState.sectorsRemaining = slowReq->sectorCount;

				if (slowReq->type == SD_REQ_WRITE) {
					startSectorWrite();
				}
				else {
					startSectorRead();
				}
				return;
			}

			/* Nothing left */
			sState.state = SD_STATE_IDLE;
			return;
		}

		/* Continue current request — start next sector */
		if (sState.currentReq->type == SD_REQ_WRITE) {
			startSectorWrite();
		}
		else {
			startSectorRead();
		}
		return;
	}

	/* Current request fully done */
	completeRequest(sState.currentReq, 0);
	if (sState.currentIsFast) {
		fastQueueDequeue();
	}
	else {
		slowQueueDequeue();
	}

	/* Pick next request */
	bool isFast;
	SdRequest* next = pickNextRequest(&isFast);
	if (next == NULL) {
		sState.state = SD_STATE_IDLE;
		return;
	}

	sState.currentReq = next;
	sState.currentIsFast = isFast;
	sState.currentSector = next->sector;
	sState.currentBuffer = next->buffer;
	sState.sectorsRemaining = next->sectorCount;

	if (next->type == SD_REQ_WRITE) {
		startSectorWrite();
	}
	else {
		startSectorRead();
	}
}

/* ---- Handle errors ---- */
static void handleError(void) {
	SDHNDL* hndl = sState.hndl;
	int32_t error = hndl->error;

	/* Soft reset the SDHI controller */
	unsigned short optBack = sd_inp(hndl, SD_OPTION);
	sd_outp(hndl, SOFT_RST, 0x0006);
	sd_outp(hndl, SOFT_RST, 0x0007);
	sd_outp(hndl, SD_OPTION, optBack);

	/* Clean up DMA */
	sddev_disable_dma(hndl->sd_port);
	sd_outp(hndl, CC_EXT_MODE, (unsigned short)(sd_inp(hndl, CC_EXT_MODE) & ~CC_EXT_MODE_DMASDRW));
	sd_outp(hndl, EXT_SWAP, 0x0000);

	/* Clear all interrupt flags and masks */
	_sd_clear_info(hndl, SD_INFO1_MASK_TRNS_RESP, 0x837F);
	_sd_clear_int_mask(hndl, SD_INFO1_MASK_TRNS_RESP, 0x837F);

	hndl->int_info1 = 0;
	hndl->int_info2 = 0;
	hndl->error = SD_OK;

	/* Notify requester of failure */
	if (sState.currentReq != NULL) {
		completeRequest(sState.currentReq, error);
		if (sState.currentIsFast) {
			fastQueueDequeue();
		}
		else {
			slowQueueDequeue();
		}
	}

	/* Try next request */
	bool isFast;
	SdRequest* next = pickNextRequest(&isFast);
	if (next == NULL) {
		sState.state = SD_STATE_IDLE;
		return;
	}

	sState.currentReq = next;
	sState.currentIsFast = isFast;
	sState.currentSector = next->sector;
	sState.currentBuffer = next->buffer;
	sState.sectorsRemaining = next->sectorCount;

	if (next->type == SD_REQ_WRITE) {
		startSectorWrite();
	}
	else {
		startSectorRead();
	}
}

/* ========================================================================
 * Main ISR entry point — called from the SDHI interrupt handler.
 * Each call advances the state machine by one step.
 * ======================================================================== */

void sdAsyncISR(void) {
	if (!sState.active) {
		return;
	}

	SDHNDL* hndl = sState.hndl;

	/* Note: sd_int_handler() has already called _sd_get_int() which reads
	 * and clears the hardware interrupt flags, accumulating them into
	 * hndl->int_info1 and hndl->int_info2. We just read those fields. */

	switch (sState.state) {
	case SD_STATE_IDLE: {
		/* Check if new work arrived */
		bool isFast;
		SdRequest* req = pickNextRequest(&isFast);
		if (req == NULL) {
			return; /* Nothing to do */
		}
		sState.currentReq = req;
		sState.currentIsFast = isFast;
		sState.currentSector = req->sector;
		sState.currentBuffer = req->buffer;
		sState.sectorsRemaining = req->sectorCount;

		if (req->type == SD_REQ_WRITE) {
			startSectorWrite();
		}
		else {
			startSectorRead();
		}
		break;
	}

	case SD_STATE_CMD_SENT:
		/* Command response received */
		if (hndl->int_info1 & SD_INFO1_MASK_RESP) {
			handleCmdResponse();
		}
		else if (hndl->int_info2 & SD_INFO2_MASK_ERR) {
			_sd_check_info2_err(hndl);
			sState.state = SD_STATE_ERROR;
			handleError();
		}
		break;

	case SD_STATE_DMA_RUNNING:
		/* DMA transfer complete — check the DMA end flag */
		handleDmaComplete();
		break;

	case SD_STATE_ALL_END_WAIT:
		/* "All end" interrupt received */
		if (hndl->int_info1 & SD_INFO1_MASK_DATA_TRNS) {
			handleAllEnd();
		}
		if (sState.state == SD_STATE_SECTOR_DONE) {
			handleSectorDone();
		}
		break;

	case SD_STATE_SECTOR_DONE:
		handleSectorDone();
		break;

	case SD_STATE_ERROR:
		handleError();
		break;
	}
}

/* ========================================================================
 * Kick function — starts the ISR state machine if it's idle and there's
 * work in the queue. Called from task context after enqueueing a request.
 * Triggers an SDHI interrupt to wake the state machine.
 * ======================================================================== */

static void sdAsyncKick(void) {
	if (sState.state != SD_STATE_IDLE) {
		return; /* Already running */
	}

	/* TEMPORARY DIAGNOSTIC: Instead of using our ISR state machine,
	 * do a synchronous single-sector read using sd_read_sect to verify
	 * the hardware works. If this succeeds, our ISR register sequence
	 * is the problem. Process the first request from whichever queue
	 * has work. */
	bool isFast;
	SdRequest* req = pickNextRequest(&isFast);
	if (req == NULL) {
		return;
	}

	/* Temporarily disable async so sd_read_sect uses the semaphore path */
	sState.active = false;

	if (req->type == SD_REQ_WRITE) {
		int err = sd_write_sect(sState.hndl->sd_port, req->buffer, req->sector, req->sectorCount, 0x0001u);
		completeRequest(req, err);
	}
	else {
		int err = sd_read_sect(sState.hndl->sd_port, req->buffer, req->sector, req->sectorCount);
		completeRequest(req, err);
	}

	if (isFast) {
		fastQueueDequeue();
	}
	else {
		slowQueueDequeue();
	}

	sState.active = true;

	/* Check if there's more work — recursively kick */
	if (!fastQueueEmpty() || !slowQueueEmpty()) {
		sdAsyncKick();
	}
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void sdAsyncInit(void) {
	/* Zero out queues and state */
	sFastQueue.head = 0;
	sFastQueue.tail = 0;
	sSlowQueue.head = 0;
	sSlowQueue.tail = 0;

	sState.state = SD_STATE_IDLE;
	sState.currentReq = NULL;
	sState.active = false;
	sState.hndl = _sd_get_hndls(SD_PORT);
	sState.fastCounter = 0;
	sState.dma64 = SD_MODE_DMA;
}

void sdAsyncStart(void) {
	extern void freezeWithError(const char* errmsg);

	if (sState.hndl == NULL) {
		freezeWithError("HNUL");
	}

	/* Do a synchronous dummy read to put the card in transfer state
	 * with the clock running. This uses the old semaphore path
	 * (active is still false). sd_read_sect internally:
	 * 1. Enables clock (_sd_set_clock ENABLE)
	 * 2. Sends CMD13 to verify card is in TRAN state
	 * 3. Reads one sector
	 * 4. Disables clock (_sd_set_clock DISABLE)
	 *
	 * We set active=true BEFORE the read returns so that step 4's
	 * clock disable is blocked by our patch in sd_util.c.
	 * The semaphore-based ISR path still works because we check
	 * sdAsyncIsActive() in the ISR handler — but the flag is only
	 * checked AFTER sd_int_handler accumulates flags, and the
	 * semaphore give still happens in the else branch during this
	 * transitional read.
	 *
	 * Actually, we can't do this — setting active=true while
	 * sd_read_sect is running would cause the ISR to call
	 * sdAsyncISR instead of giving the semaphore, which would
	 * break the synchronous wait.
	 *
	 * Instead: just enable the clock manually and leave active=false
	 * until after the read completes. Then set active=true. The
	 * clock disable at the end of sd_read_sect will turn it off,
	 * but we immediately re-enable it after. */
	{
		uint8_t dummyBuf[512];
		sd_read_sect(sState.hndl->sd_port, dummyBuf, 0, 1);
	}
	/* Clock was just disabled by sd_read_sect. Re-enable it.
	 * Now set active=true so it won't be disabled again. */
	sState.active = true;
	_sd_set_clock(sState.hndl, (int)sState.hndl->csd_tran_speed, SD_CLOCK_ENABLE);
}

bool sdAsyncIsActive(void) {
	return sState.active;
}

bool sdAsyncReadCluster(uint32_t sector, uint8_t* buffer, uint32_t sectorCount, SdAsyncCallback callback,
                        void* userData) {
	SdRequest req;
	req.type = SD_REQ_READ_CLUSTER;
	req.sector = sector;
	req.buffer = buffer;
	req.sectorCount = sectorCount;
	req.completionSem = NULL;
	req.resultPtr = NULL;
	req.callback = callback;
	req.userData = userData;

	/* Disable interrupts briefly to prevent the ISR from reading
	 * a partially-written queue entry */
	UBaseType_t savedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
	bool ok = fastQueueEnqueue(&req);
	taskEXIT_CRITICAL_FROM_ISR(savedInterruptStatus);

	if (ok) {
		sdAsyncKick();
	}
	return ok;
}

/* Static storage for slow-path semaphores.
 * Each slow-path caller gets a per-task semaphore. Since only the app task
 * and cluster loader do FatFS operations, 2 semaphores suffice. */
static StaticSemaphore_t sSlowSemStorage[SD_SLOW_QUEUE_SIZE];
static SemaphoreHandle_t sSlowSemHandles[SD_SLOW_QUEUE_SIZE];
static volatile uint32_t sSlowSemIndex = 0;

static SemaphoreHandle_t acquireSlowSemaphore(void) {
	/* Simple round-robin allocation of pre-created semaphores */
	UBaseType_t savedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
	uint32_t idx = sSlowSemIndex;
	sSlowSemIndex = (sSlowSemIndex + 1) % SD_SLOW_QUEUE_SIZE;
	taskEXIT_CRITICAL_FROM_ISR(savedInterruptStatus);

	if (sSlowSemHandles[idx] == NULL) {
		sSlowSemHandles[idx] = xSemaphoreCreateBinaryStatic(&sSlowSemStorage[idx]);
	}
	/* Ensure semaphore starts taken (binary sem starts as "empty") */
	xSemaphoreTake(sSlowSemHandles[idx], 0);
	return sSlowSemHandles[idx];
}

int32_t sdAsyncSyncRead(uint32_t sector, uint8_t* buffer, uint32_t sectorCount) {
	volatile int32_t result = 0;
	SemaphoreHandle_t sem = acquireSlowSemaphore();

	SdRequest req;
	req.type = SD_REQ_READ;
	req.sector = sector;
	req.buffer = buffer;
	req.sectorCount = sectorCount;
	req.completionSem = (void*)sem;
	req.resultPtr = &result;
	req.callback = NULL;
	req.userData = NULL;

	UBaseType_t savedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
	bool ok = slowQueueEnqueue(&req);
	taskEXIT_CRITICAL_FROM_ISR(savedInterruptStatus);

	if (!ok) {
		return SD_ERR; /* Queue full */
	}

	sdAsyncKick();

	/* Block with timeout — if the ISR doesn't complete within 5 seconds,
	 * show hardware state for diagnosis. */
	if (xSemaphoreTake(sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
		extern void freezeWithError(const char* errmsg);
		extern volatile uint32_t sdAsyncISRCount;
		SDHNDL* hndl = sState.hndl;

		char buf[5];
		unsigned short hw_info1 = sd_inp(hndl, SD_INFO1);
		unsigned short clk = sd_inp(hndl, SD_CLK_CTRL);

		/* First char: state indicator */
		if (sdAsyncISRCount > 0) {
			buf[0] = 'I'; /* ISR fired but state machine stuck */
		}
		else if (hw_info1 & 0x0001) {
			buf[0] = 'R'; /* RESP flag set but interrupt not delivered */
		}
		else if (clk & 0x0100) {
			buf[0] = 'S'; /* Clock on, no response — card not responding */
		}
		else {
			buf[0] = 'C'; /* Clock off — fundamental issue */
		}
		/* Remaining: ISR count */
		buf[1] = '0' + (sdAsyncISRCount / 100) % 10;
		buf[2] = '0' + (sdAsyncISRCount / 10) % 10;
		buf[3] = '0' + sdAsyncISRCount % 10;
		buf[4] = 0;
		freezeWithError(buf);
	}

	return result;
}

int32_t sdAsyncSyncWrite(uint32_t sector, const uint8_t* buffer, uint32_t sectorCount) {
	volatile int32_t result = 0;
	SemaphoreHandle_t sem = acquireSlowSemaphore();

	SdRequest req;
	req.type = SD_REQ_WRITE;
	req.sector = sector;
	req.buffer = (uint8_t*)buffer; /* Cast away const — the ISR reads from this buffer */
	req.sectorCount = sectorCount;
	req.completionSem = (void*)sem;
	req.resultPtr = &result;
	req.callback = NULL;
	req.userData = NULL;

	UBaseType_t savedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
	bool ok = slowQueueEnqueue(&req);
	taskEXIT_CRITICAL_FROM_ISR(savedInterruptStatus);

	if (!ok) {
		return SD_ERR;
	}

	sdAsyncKick();

	if (xSemaphoreTake(sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
		extern void freezeWithError(const char* errmsg);
		freezeWithError("SDTW"); /* SD async write timeout */
	}

	return result;
}

#endif /* USE_FREERTOS */
