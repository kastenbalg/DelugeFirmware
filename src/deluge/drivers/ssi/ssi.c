/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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

#include "drivers/ssi/ssi.h"
#include "RZA1/system/iodefines/dmac_iodefine.h"
#include "RZA1/system/iodefines/ssif_iodefine.h"
#include "definitions.h"

int32_t ssiTxBuffer[SSI_TX_BUFFER_NUM_SAMPLES * NUM_MONO_OUTPUT_CHANNELS] __attribute__((aligned(CACHE_LINE_SIZE)));

PLACE_SDRAM_DATA int32_t ssiRxBuffer[SSI_RX_BUFFER_NUM_SAMPLES * NUM_MONO_INPUT_CHANNELS]
    __attribute__((aligned(CACHE_LINE_SIZE)));

/*
 * Original config word from the single-descriptor design:
 *   0b10000001001000100010001000101000 | DMA_LVL_FOR_SSI | (SSI_TX_DMA_CHANNEL & 7)
 *
 * Bit 24 (DEM) = 1 means transfer-end interrupt is MASKED (disabled).
 * For interrupt-driven operation, we clear bit 24 to ENABLE the interrupt.
 */
#define SSI_TX_DMA_CONFIG_BASE (0b10000001001000100010001000101000 | DMA_LVL_FOR_SSI | (SSI_TX_DMA_CHANNEL & 7))
#define SSI_TX_DMA_CONFIG_IRQ (SSI_TX_DMA_CONFIG_BASE & ~(1u << 24)) /* Clear DEM: enable transfer-end interrupt */

#ifdef USE_FREERTOS
/*
 * Ping-pong DMA descriptors for interrupt-driven audio output.
 *
 * Two descriptors (A and B) each cover half the TX buffer (128 samples).
 * A links to B, B links to A, creating continuous ping-pong operation.
 * Each descriptor triggers a DMA transfer-end interrupt when its half
 * completes (~2.9ms at 44.1kHz), waking the audio task to render.
 *
 * Half-buffer size: 128 samples * 2 channels * 4 bytes = 1024 bytes
 */
#define HALF_TX_BUFFER_BYTES (SSI_TX_BUFFER_NUM_SAMPLES * NUM_MONO_OUTPUT_CHANNELS * sizeof(int32_t) / 2)
#define HALF_TX_BUFFER_SAMPLES (SSI_TX_BUFFER_NUM_SAMPLES / 2)

/* Forward declaration for cross-referencing between descriptors */
extern const uint32_t ssiDmaTxLinkDescriptorB[];

const uint32_t ssiDmaTxLinkDescriptorA[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                    // Header
    (uint32_t)ssiTxBuffer,                     // Source: first half of buffer
    (uint32_t)&SSIF(SSI_CHANNEL).SSIFTDR.LONG, // Destination: SSI FIFO
    HALF_TX_BUFFER_BYTES,                      // Transaction size: half buffer
    SSI_TX_DMA_CONFIG_IRQ,                     // Config: interrupt enabled
    0,                                         // Interval
    0,                                         // Extension
    (uint32_t)ssiDmaTxLinkDescriptorB          // Next: descriptor B
};

const uint32_t ssiDmaTxLinkDescriptorB[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                                                    // Header
    (uint32_t)&ssiTxBuffer[HALF_TX_BUFFER_SAMPLES * NUM_MONO_OUTPUT_CHANNELS], // Source: second half
    (uint32_t)&SSIF(SSI_CHANNEL).SSIFTDR.LONG,                                 // Destination: SSI FIFO
    HALF_TX_BUFFER_BYTES,                                                      // Transaction size: half buffer
    SSI_TX_DMA_CONFIG_IRQ,                                                     // Config: interrupt enabled
    0,                                                                         // Interval
    0,                                                                         // Extension
    (uint32_t)ssiDmaTxLinkDescriptorA                                          // Next: descriptor A (ping-pong)
};

/* Alias for compatibility — ssiInit uses ssiDmaTxLinkDescriptor */
const uint32_t* ssiDmaTxLinkDescriptor = ssiDmaTxLinkDescriptorA;

/* Track which half just completed (0 = first half done, 1 = second half done) */
volatile uint8_t ssiTxHalfComplete = 0;

#else
/* Original single-descriptor design for non-FreeRTOS builds */
const uint32_t ssiDmaTxLinkDescriptor[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                    // Header
    (uint32_t)ssiTxBuffer,                     // Source address
    (uint32_t)&SSIF(SSI_CHANNEL).SSIFTDR.LONG, // Destination address
    sizeof(ssiTxBuffer),                       // Transaction size
    SSI_TX_DMA_CONFIG_BASE,                    // Config (interrupt masked)
    0,                                         // Interval
    0,                                         // Extension
    (uint32_t)ssiDmaTxLinkDescriptor           // Next link address (this one again)
};
#endif /* USE_FREERTOS */

const uint32_t ssiDmaRxLinkDescriptor[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                                                          // Header
    (uint32_t)&SSIF(SSI_CHANNEL).SSIFRDR.LONG,                                       // Source address
    (uint32_t)ssiRxBuffer,                                                           // Destination address
    sizeof(ssiRxBuffer),                                                             // Transaction size
    0b10000001000100100010001000100000 | DMA_LVL_FOR_SSI | (SSI_RX_DMA_CHANNEL & 7), // Config
    0,                                                                               // Interval
    0,                                                                               // Extension
    (uint32_t)ssiDmaRxLinkDescriptor // Next link address (this one again)
};

void* getTxBufferCurrentPlace() {
	return (void*)((DMACn(SSI_TX_DMA_CHANNEL).CRSA_n & ~((NUM_MONO_OUTPUT_CHANNELS * 4) - 1)) + UNCACHED_MIRROR_OFFSET);
}

void* getRxBufferCurrentPlace() {
	return (void*)((DMACn(SSI_RX_DMA_CHANNEL).CRDA_n & ~((NUM_MONO_INPUT_CHANNELS * 4) - 1)) + UNCACHED_MIRROR_OFFSET);
}

int32_t* getTxBufferStart() {
	return (int32_t*)((uint32_t)&ssiTxBuffer[0] + UNCACHED_MIRROR_OFFSET);
}

int32_t* getTxBufferEnd() {
	return (int32_t*)((uint32_t)&ssiTxBuffer[SSI_TX_BUFFER_NUM_SAMPLES * NUM_MONO_OUTPUT_CHANNELS]
	                  + UNCACHED_MIRROR_OFFSET);
}

#ifdef USE_FREERTOS
int32_t* getTxBufferHalfPoint() {
	return (int32_t*)((uint32_t)&ssiTxBuffer[HALF_TX_BUFFER_SAMPLES * NUM_MONO_OUTPUT_CHANNELS]
	                  + UNCACHED_MIRROR_OFFSET);
}
#endif

inline void clearTxBuffer() {
	int output = 0;
	for (int32_t* address = getTxBufferStart(); address < getTxBufferEnd(); address++) {
		*address = output;
		output = !output;
	}
}
int32_t* getRxBufferStart() {
	return (int32_t*)((uint32_t)&ssiRxBuffer[0] + UNCACHED_MIRROR_OFFSET);
}

int32_t* getRxBufferEnd() {
	return (int32_t*)((uint32_t)&ssiRxBuffer[SSI_RX_BUFFER_NUM_SAMPLES * NUM_MONO_INPUT_CHANNELS]
	                  + UNCACHED_MIRROR_OFFSET);
}
