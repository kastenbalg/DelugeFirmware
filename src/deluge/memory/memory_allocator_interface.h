#pragma once

// Lightweight allocation interface — include this instead of general_memory_allocator.h when you only need
// alloc/dealloc/extend/shorten free functions. This avoids pulling in MemoryRegion, CacheManager, definitions_cxx.hpp,
// etc.
#include <cstdint>

class Stealable;
enum class StealableQueue;

// ── Primary allocation ───────────────────────────────────────────────
// allocInternal: tries internal (small then main) → external → stealable fallback
void* allocInternal(uint32_t requiredSize, void* thingNotToStealFrom = nullptr);

// allocExternal: tries external (small then main) → stealable fallback
void* allocExternal(uint32_t requiredSize, void* thingNotToStealFrom = nullptr);

// allocStealable: allocates in the stealable region only
void* allocStealable(uint32_t requiredSize, void* thingNotToStealFrom = nullptr);

// ── Direct region allocation (no stealable fallback) ─────────────────
void* allocExternalDirect(uint32_t requiredSize);
void deallocExternalDirect(void* address);

// ── Deallocation ─────────────────────────────────────────────────────
extern "C" {
void* delugeAlloc(unsigned int requiredSize, bool mayUseOnChipRam = true);
void delugeDealloc(void* address);
}

// ── Query / resize ───────────────────────────────────────────────────
uint32_t getAllocatedSize(void* address);
uint32_t shortenRight(void* address, uint32_t newSize);
uint32_t shortenLeft(void* address, uint32_t amountToShorten, uint32_t numBytesToMoveRightIfSuccessful = 0);
void memoryExtend(void* address, uint32_t minAmountToExtend, uint32_t idealAmountToExtend,
                  uint32_t* getAmountExtendedLeft, uint32_t* getAmountExtendedRight,
                  void* thingNotToStealFrom = nullptr);
uint32_t extendRightAsMuchAsEasilyPossible(void* address);

// ── Diagnostics ──────────────────────────────────────────────────────
void checkStack(char const* caller);
int32_t getMemoryRegion(void* address);

// ── Stealable queue management ───────────────────────────────────────
void putStealableInQueue(Stealable* stealable, StealableQueue q);
void putStealableInAppropriateQueue(Stealable* stealable);
