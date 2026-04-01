#include "definitions.h"
#include <exception>

#ifdef USE_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#endif

/* Store the last known operation context so we can display it on crash.
 * Call terminate_set_context("XXXX") before risky operations to tag
 * which subsystem was active when std::terminate fires. */
static const char* volatile sTerminateContext = "TERM";

extern "C" void terminate_set_context(const char* ctx) {
	sTerminateContext = ctx;
}

[[noreturn]] void Terminate() noexcept {
#ifdef USE_FREERTOS
	/* Identify WHICH task triggered terminate:
	 * "TA__" = Audio task, "TQ__" = Sequencer, others show original context.
	 * Last 2 chars of context preserved for identification. */
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
		extern TaskHandle_t audioTaskHandle;
		extern TaskHandle_t sequencerTaskHandle;
		TaskHandle_t current = xTaskGetCurrentTaskHandle();
		const char* ctx = sTerminateContext;
		char c2 = ctx[2] ? ctx[2] : '_';
		char c3 = ctx[3] ? ctx[3] : '_';
		static char buf[5];
		if (current == audioTaskHandle) {
			buf[0] = 'T';
			buf[1] = 'A';
			buf[2] = c2;
			buf[3] = c3;
		}
		else if (current == sequencerTaskHandle) {
			buf[0] = 'T';
			buf[1] = 'Q';
			buf[2] = c2;
			buf[3] = c3;
		}
		else {
			freezeWithError(sTerminateContext);
		}
		buf[4] = '\0';
		freezeWithError(buf);
	}
#endif
	freezeWithError(sTerminateContext);
	__builtin_unreachable();
}

namespace __cxxabiv1 {
std::terminate_handler __terminate_handler = Terminate;
}
