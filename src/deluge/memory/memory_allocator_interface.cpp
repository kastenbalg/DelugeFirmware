#include "memory/general_memory_allocator.h"

void* allocInternal(uint32_t requiredSize, void* thingNotToStealFrom) {
	return GeneralMemoryAllocator::get().alloc(requiredSize, true, false, thingNotToStealFrom);
}

void* allocExternal(uint32_t requiredSize, void* thingNotToStealFrom) {
	return GeneralMemoryAllocator::get().alloc(requiredSize, false, false, thingNotToStealFrom);
}

void* allocStealable(uint32_t requiredSize, void* thingNotToStealFrom) {
	return GeneralMemoryAllocator::get().alloc(requiredSize, false, true, thingNotToStealFrom);
}

void* allocExternalDirect(uint32_t requiredSize) {
	return GeneralMemoryAllocator::get().allocExternalDirect(requiredSize);
}

void deallocExternalDirect(void* address) {
	GeneralMemoryAllocator::get().deallocExternalDirect(address);
}

uint32_t getAllocatedSize(void* address) {
	return GeneralMemoryAllocator::get().getAllocatedSize(address);
}

uint32_t shortenRight(void* address, uint32_t newSize) {
	return GeneralMemoryAllocator::get().shortenRight(address, newSize);
}

uint32_t shortenLeft(void* address, uint32_t amountToShorten, uint32_t numBytesToMoveRightIfSuccessful) {
	return GeneralMemoryAllocator::get().shortenLeft(address, amountToShorten, numBytesToMoveRightIfSuccessful);
}

void memoryExtend(void* address, uint32_t minAmountToExtend, uint32_t idealAmountToExtend,
                  uint32_t* getAmountExtendedLeft, uint32_t* getAmountExtendedRight, void* thingNotToStealFrom) {
	GeneralMemoryAllocator::get().extend(address, minAmountToExtend, idealAmountToExtend, getAmountExtendedLeft,
	                                     getAmountExtendedRight, thingNotToStealFrom);
}

uint32_t extendRightAsMuchAsEasilyPossible(void* address) {
	return GeneralMemoryAllocator::get().extendRightAsMuchAsEasilyPossible(address);
}

void checkStack(char const* caller) {
	GeneralMemoryAllocator::get().checkStack(caller);
}

int32_t getMemoryRegion(void* address) {
	return GeneralMemoryAllocator::get().getRegion(address);
}

void putStealableInQueue(Stealable* stealable, StealableQueue q) {
	GeneralMemoryAllocator::get().putStealableInQueue(stealable, q);
}

void putStealableInAppropriateQueue(Stealable* stealable) {
	GeneralMemoryAllocator::get().putStealableInAppropriateQueue(stealable);
}
