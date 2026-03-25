#include "definitions.h"
#include <exception>

/* Store the last known operation context so we can display it on crash.
 * Call terminate_set_context("XXXX") before risky operations to tag
 * which subsystem was active when std::terminate fires. */
static const char* volatile sTerminateContext = "TERM";

extern "C" void terminate_set_context(const char* ctx) {
	sTerminateContext = ctx;
}

[[noreturn]] void Terminate() noexcept {
	freezeWithError(sTerminateContext);
	__builtin_unreachable();
}

namespace __cxxabiv1 {
std::terminate_handler __terminate_handler = Terminate;
}
