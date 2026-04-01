#include "memory/general_memory_allocator.h"
#include <new>
#ifdef USE_FREERTOS
extern "C" void freezeWithError(const char* errmsg);
#endif

// todo - make this work in unit tests, need to remove hard coded addresses in GMA
#if !IN_UNIT_TESTS
void* operator new(std::size_t n) noexcept(false) {
	// allocate on external RAM
	void* p = GeneralMemoryAllocator::get().allocExternal(n);
#ifdef USE_FREERTOS
	if (!p) {
		/* Returning nullptr from throwing operator new is undefined behavior
		 * and triggers std::terminate. Freeze with diagnostic instead. */
		freezeWithError("NWOM");
	}
#endif
	return p;
}

void operator delete(void* p) {
	delugeDealloc(p);
}
#endif
