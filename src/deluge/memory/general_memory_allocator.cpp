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

#include "memory/general_memory_allocator.h"

#include "definitions_cxx.hpp"
#include "io/debug/log.h"
#include "memory/stealable.h"
#include "processing/engines/audio_engine.h"

// these are never used directly, they're just reserving raw memory for use in the allocator  and clang tidy is unhappy
// NOLINTBEGIN
// TODO: Check if these have the right size
PLACE_INTERNAL_FRUNK char emptySpacesMemory[sizeof(EmptySpaceRecord) * 512];
PLACE_INTERNAL_FRUNK char emptySpacesMemoryInternal[sizeof(EmptySpaceRecord) * 1024];
PLACE_INTERNAL_FRUNK char emptySpacesMemoryInternalSmall[sizeof(EmptySpaceRecord) * 256];
PLACE_INTERNAL_FRUNK char emptySpacesMemoryGeneral[sizeof(EmptySpaceRecord) * 256];
PLACE_INTERNAL_FRUNK char emptySpacesMemoryGeneralSmall[sizeof(EmptySpaceRecord) * 256];
extern uint32_t __frunk_bss_end;
extern uint32_t __frunk_slack_end;
extern uint32_t __sdram_bss_start;
extern uint32_t __sdram_bss_end;
extern uint32_t __heap_start;
extern uint32_t __heap_end;
extern uint32_t program_stack_start;
extern uint32_t program_stack_end;
// NOLINTEND
GeneralMemoryAllocator::GeneralMemoryAllocator()
    : lock(false)
#ifdef USE_FREERTOS
      ,
      allocMutex(nullptr)
#endif
{
#ifdef USE_FREERTOS
	allocMutex = rtos_mutex_create(&allocMutexStorage);
#endif
	uint32_t external_small_end = EXTERNAL_MEMORY_END;
	uint32_t external_small_start = external_small_end - RESERVED_EXTERNAL_SMALL_ALLOCATOR;
	uint32_t external_end = external_small_start;
	uint32_t external_start = external_small_start - RESERVED_EXTERNAL_ALLOCATOR;
	uint32_t stealable_end = external_start;
	// NOLINTBEGIN
	// clang tidy hates both reinterpret and c style casts but linker output is only meaningful when taking address
	auto stealable_start = (uint32_t)&__sdram_bss_end;

	auto internal_small_start = (uint32_t)&__frunk_bss_end;
	auto internal_small_end = (uint32_t)&__frunk_slack_end;
	auto internal_start = (uint32_t)&__heap_start;
	auto internal_end = (uint32_t)&program_stack_start;
	// NOLINTEND
	regions[MEMORY_REGION_STEALABLE].name = "stealable";
	regions[MEMORY_REGION_INTERNAL].name = "internal";
	regions[MEMORY_REGION_EXTERNAL].name = "external";
	regions[MEMORY_REGION_EXTERNAL_SMALL].name = "small external";
	regions[MEMORY_REGION_INTERNAL_SMALL].name = "small internal";

	regions[MEMORY_REGION_STEALABLE].setup(emptySpacesMemory, sizeof(emptySpacesMemory), stealable_start, stealable_end,
	                                       &cacheManager);
	regions[MEMORY_REGION_EXTERNAL].setup(emptySpacesMemoryGeneral, sizeof(emptySpacesMemoryGeneral), external_start,
	                                      external_end, nullptr);
	regions[MEMORY_REGION_EXTERNAL_SMALL].setup(emptySpacesMemoryGeneralSmall, sizeof(emptySpacesMemoryGeneralSmall),
	                                            external_small_start, external_small_end, nullptr);
	regions[MEMORY_REGION_EXTERNAL_SMALL].minAlign_ = 16;
	regions[MEMORY_REGION_EXTERNAL_SMALL].pivot_ = 64;
	regions[MEMORY_REGION_INTERNAL].setup(emptySpacesMemoryInternal, sizeof(emptySpacesMemoryInternal), internal_start,
	                                      internal_end, nullptr);

	regions[MEMORY_REGION_INTERNAL_SMALL].setup(emptySpacesMemoryInternalSmall, sizeof(emptySpacesMemoryInternalSmall),
	                                            internal_small_start, internal_small_end, nullptr);
	regions[MEMORY_REGION_INTERNAL_SMALL].minAlign_ = 16;
	regions[MEMORY_REGION_INTERNAL_SMALL].pivot_ = 64;
}

#ifdef USE_FREERTOS
void GeneralMemoryAllocator::lockMutex() {
	rtos_mutex_lock(allocMutex);
}
void GeneralMemoryAllocator::unlockMutex() {
	rtos_mutex_unlock(allocMutex);
}
#endif

constexpr size_t kInternalSwitchSize = 128;
constexpr size_t kExternalSwitchSize = 128;
int32_t closestDistance = 2147483647;

void GeneralMemoryAllocator::checkStack(char const* caller) {
#if ALPHA_OR_BETA_VERSION

#ifdef USE_FREERTOS
	/* Under FreeRTOS the task runs on its own stack, not the linker-defined
	 * program stack.  FreeRTOS's own stack overflow detection
	 * (configCHECK_FOR_STACK_OVERFLOW) handles this instead. */
	(void)caller;
#else
	char a;

	int32_t distance = (int32_t)&a - (uint32_t)&program_stack_start;
	if (distance < closestDistance) {
		closestDistance = distance;

		D_PRINTLN("%d bytes in stack %d free bytes in stack at %s", (uint32_t)&program_stack_end - (int32_t)&a,
		          distance, caller);
		if (distance < 200) {
			FREEZE_WITH_ERROR("E338");
			D_PRINTLN("COLLISION");
		}
	}
#endif

#endif
}

#if TEST_GENERAL_MEMORY_ALLOCATION
uint32_t totalMallocTime = 0;
int32_t numMallocTimes = 0;
#endif
extern "C" void* delugeAlloc(unsigned int requiredSize, bool mayUseOnChipRam) {
	return GeneralMemoryAllocator::get().alloc(requiredSize, mayUseOnChipRam, false, nullptr);
}
extern "C" void delugeDealloc(void* address) {
#ifdef IN_UNIT_TESTS
	free(address);
#else
	GeneralMemoryAllocator::get().dealloc(address);
#endif
}
/* Unlocked helpers — caller must hold allocMutex and set lock=true */
static void* allocExternalUnlocked(MemoryRegion* regions, uint32_t requiredSize) {
	void* address = nullptr;
	if (requiredSize < kExternalSwitchSize) {
		address = regions[MEMORY_REGION_EXTERNAL_SMALL].alloc(requiredSize, false, NULL);
	}
	if (address == nullptr) {
		address = regions[MEMORY_REGION_EXTERNAL].alloc(requiredSize, false, NULL);
	}
	return address;
}

static void* allocInternalUnlocked(MemoryRegion* regions, uint32_t requiredSize) {
	void* address = nullptr;
	if (requiredSize < kInternalSwitchSize) {
		address = regions[MEMORY_REGION_INTERNAL_SMALL].alloc(requiredSize, false, NULL);
	}
	if (address == nullptr) {
		address = regions[MEMORY_REGION_INTERNAL].alloc(requiredSize, false, NULL);
	}
	return address;
}

/* Public locked entry points */
void* GeneralMemoryAllocator::allocExternal(uint32_t requiredSize) {
	if (lock) {
		return nullptr;
	}
	lockMutex();
	lock = true;
	void* address = allocExternalUnlocked(regions, requiredSize);
	lock = false;
	unlockMutex();
	return address;
}

void* GeneralMemoryAllocator::allocInternal(uint32_t requiredSize) {
	if (lock) {
		return nullptr;
	}
	lockMutex();
	lock = true;
	void* address = allocInternalUnlocked(regions, requiredSize);
	lock = false;
	unlockMutex();
	return address;
}

void GeneralMemoryAllocator::deallocExternal(void* address) {
	lockMutex();
	regions[getRegion(address)].dealloc(address);
	unlockMutex();
}

// Watch the heck out - in the older V3.1 branch, this had one less argument - makeStealable was missing - so in code
// from there, thingNotToStealFrom could be interpreted as makeStealable! requiredSize 0 means get biggest allocation
// available.
void* GeneralMemoryAllocator::alloc(uint32_t requiredSize, bool mayUseOnChipRam, bool makeStealable,
                                    void* thingNotToStealFrom) {

	if (lock) {
		return nullptr; // Prevent any weird loops in freeSomeStealableMemory(), which mostly would only be bad cos they
		                // could extend the stack an unspecified amount
	}

	lockMutex();
	lock = true;

	void* address = nullptr;

	// Only allow allocating stealables in stelable region
	if (!makeStealable) {
		// If internal is allowed, try that first
		if (mayUseOnChipRam) {
			address = allocInternalUnlocked(regions, requiredSize);

			if (address != nullptr) {
				lock = false;
				unlockMutex();
				return address;
			}

			AudioEngine::logAction("internal allocation failed");
		}

		// Second try external region
		address = allocExternalUnlocked(regions, requiredSize);

		if (address) {
			lock = false;
			unlockMutex();
			return address;
		}

		AudioEngine::logAction("external allocation failed");

		D_PRINTLN("Dire memory, resorting to stealable area");
	}

#if TEST_GENERAL_MEMORY_ALLOCATION
	if (requiredSize < 1) {
		D_PRINTLN("alloc too little a bit");
		while (1) {}
	}
#endif

	address = regions[MEMORY_REGION_STEALABLE].alloc(requiredSize, makeStealable, thingNotToStealFrom);
	lock = false;
	unlockMutex();
	return address;
}

uint32_t GeneralMemoryAllocator::getAllocatedSize(void* address) {
	uint32_t* header = (uint32_t*)((uint32_t)address - 4);
	return (*header & SPACE_SIZE_MASK);
}

int32_t GeneralMemoryAllocator::getRegion(void* address) {
	uint32_t value = (uint32_t)address;
	if (value >= regions[MEMORY_REGION_INTERNAL].start && value < regions[MEMORY_REGION_INTERNAL].end) {
		return MEMORY_REGION_INTERNAL;
	}
	else if (value >= regions[MEMORY_REGION_STEALABLE].start && value < regions[MEMORY_REGION_STEALABLE].end) {
		return MEMORY_REGION_STEALABLE;
	}
	else if (value >= regions[MEMORY_REGION_EXTERNAL].start && value < regions[MEMORY_REGION_EXTERNAL].end) {
		return MEMORY_REGION_EXTERNAL;
	}
	else if (value >= regions[MEMORY_REGION_EXTERNAL_SMALL].start
	         && value < regions[MEMORY_REGION_EXTERNAL_SMALL].end) {
		return MEMORY_REGION_EXTERNAL_SMALL;
	}
	else if (value >= regions[MEMORY_REGION_INTERNAL_SMALL].start
	         && value < regions[MEMORY_REGION_INTERNAL_SMALL].end) {
		return MEMORY_REGION_INTERNAL_SMALL;
	}

	FREEZE_WITH_ERROR("E339");
	return 0;
}

// Returns new size
uint32_t GeneralMemoryAllocator::shortenRight(void* address, uint32_t newSize) {
	lockMutex();
	uint32_t result = regions[getRegion(address)].shortenRight(address, newSize);
	unlockMutex();
	return result;
}

// Returns how much it was shortened by
uint32_t GeneralMemoryAllocator::shortenLeft(void* address, uint32_t amountToShorten,
                                             uint32_t numBytesToMoveRightIfSuccessful) {
	lockMutex();
	uint32_t result =
	    regions[getRegion(address)].shortenLeft(address, amountToShorten, numBytesToMoveRightIfSuccessful);
	unlockMutex();
	return result;
}

void GeneralMemoryAllocator::extend(void* address, uint32_t minAmountToExtend, uint32_t idealAmountToExtend,
                                    uint32_t* __restrict__ getAmountExtendedLeft,
                                    uint32_t* __restrict__ getAmountExtendedRight, void* thingNotToStealFrom) {

	*getAmountExtendedLeft = 0;
	*getAmountExtendedRight = 0;

	if (lock) {
		return;
	}

	lockMutex();
	lock = true;
	regions[getRegion(address)].extend(address, minAmountToExtend, idealAmountToExtend, getAmountExtendedLeft,
	                                   getAmountExtendedRight, thingNotToStealFrom);
	lock = false;
	unlockMutex();
}

uint32_t GeneralMemoryAllocator::extendRightAsMuchAsEasilyPossible(void* address) {
	lockMutex();
	uint32_t result = regions[getRegion(address)].extendRightAsMuchAsEasilyPossible(address);
	unlockMutex();
	return result;
}

void GeneralMemoryAllocator::dealloc(void* address) {
	if (address == nullptr) [[unlikely]] {
		return;
	}
	lockMutex();
	regions[getRegion(address)].dealloc(address);
	unlockMutex();
}

void GeneralMemoryAllocator::putStealableInQueue(Stealable* stealable, StealableQueue q) {
	lockMutex();
	MemoryRegion& region = regions[getRegion(stealable)];
	region.cache_manager().QueueForReclamation(q, stealable);
	unlockMutex();
}

void GeneralMemoryAllocator::putStealableInAppropriateQueue(Stealable* stealable) {
	StealableQueue q = stealable->getAppropriateQueue();
	putStealableInQueue(stealable, q);
}
