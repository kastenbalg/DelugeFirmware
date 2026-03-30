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

/*
 * L2 (PL310) cache maintenance and combined L1+L2 DMA cache maintenance.
 *
 * The RZ/A1L's DMAC is a north-main-bus master that bypasses the PL310 L2 cache,
 * accessing SDRAM directly through the bus bridge. The CPU accesses SDRAM through
 * the L2. This means DMA transfers can create stale L2 cache lines that must be
 * explicitly maintained.
 *
 * Ordering rules per ARM L2C-310 TRM (DDI0246F §3.3.10):
 *   Invalidate (after DMA writes to memory): L2 first, then L1
 *   Clean (before DMA reads from memory):    L1 first, then L2
 */

#include "RZA1/system/iodefine.h"
#include "definitions.h"
#include <stdint.h>

/* L1-only assembly functions (renamed from v7_dma_inv_range / v7_dma_flush_range) */
extern void v7_l1_dma_inv_range(uintptr_t start, uintptr_t end);
extern void v7_l1_dma_flush_range(uintptr_t start, uintptr_t end);

/* Invalidate L2 cache lines covering [start, end) by physical address. */
void l2c_inv_range(uintptr_t start, uintptr_t end) {
	uintptr_t addr = start & ~((uintptr_t)CACHE_LINE_SIZE - 1);
	while (addr < end) {
		L2C.REG7_INV_PA = (uint32_t)addr;
		addr += CACHE_LINE_SIZE;
	}
	L2C.REG7_CACHE_SYNC = 0;
}

/* Clean (write-back) L2 cache lines covering [start, end) by physical address. */
void l2c_clean_range(uintptr_t start, uintptr_t end) {
	uintptr_t addr = start & ~((uintptr_t)CACHE_LINE_SIZE - 1);
	while (addr < end) {
		L2C.REG7_CLEAN_PA = (uint32_t)addr;
		addr += CACHE_LINE_SIZE;
	}
	L2C.REG7_CACHE_SYNC = 0;
}

/* Clean and invalidate L2 cache lines covering [start, end) by physical address. */
void l2c_clean_inv_range(uintptr_t start, uintptr_t end) {
	uintptr_t addr = start & ~((uintptr_t)CACHE_LINE_SIZE - 1);
	while (addr < end) {
		L2C.REG7_CLEAN_INV_PA = (uint32_t)addr;
		addr += CACHE_LINE_SIZE;
	}
	L2C.REG7_CACHE_SYNC = 0;
}

/*
 * Combined L1+L2 DMA invalidation — call after DMA writes to memory.
 *
 * TRM ordering: L2 invalidate first, then L1. This prevents the L2 from
 * re-filling L1 with stale data between the L1 invalidate and L2 invalidate.
 *
 * Replaces the old L1-only v7_dma_inv_range transparently.
 */
void v7_dma_inv_range(uintptr_t start, uintptr_t end) {
	l2c_inv_range(start, end);
	v7_l1_dma_inv_range(start, end);
}

/*
 * Combined L1+L2 DMA flush (clean) — call before DMA reads from memory.
 *
 * TRM ordering: L1 clean first (pushes dirty lines to L2/SDRAM), then L2 clean
 * (pushes any remaining dirty L2 lines to SDRAM so DMA sees current data).
 *
 * Replaces the old L1-only v7_dma_flush_range transparently.
 */
void v7_dma_flush_range(uintptr_t start, uintptr_t end) {
	v7_l1_dma_flush_range(start, end);
	l2c_clean_range(start, end);
}
