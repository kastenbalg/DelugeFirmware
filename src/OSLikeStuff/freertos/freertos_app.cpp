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
 * FreeRTOS application wrapper for Phase 1 migration.
 *
 * Creates a single FreeRTOS task that runs the existing cooperative scheduler
 * loop verbatim. All task timing, yield(), resource locks, and scheduling
 * decisions are unchanged. FreeRTOS provides only the kernel infrastructure
 * (tick timer, idle task) as foundation for Phase 2 multi-task migration.
 */

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "task.h"

/* Static allocation for the main scheduler task.
 * 8192 words = 32KB, matching the existing PROGRAM_STACK_SIZE. */
static StaticTask_t sMainTaskTCB;
static StackType_t sMainTaskStack[8192];

/* Static allocation for the idle task (required when configSUPPORT_STATIC_ALLOCATION=1). */
static StaticTask_t sIdleTaskTCB;
static StackType_t sIdleTaskStack[configMINIMAL_STACK_SIZE];

/* FreeRTOS requires this callback when static allocation is enabled. */
extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t** ppxIdleTaskTCBBuffer, StackType_t** ppxIdleTaskStackBuffer,
                                              uint32_t* pulIdleTaskStackSize) {
	*ppxIdleTaskTCBBuffer = &sIdleTaskTCB;
	*ppxIdleTaskStackBuffer = sIdleTaskStack;
	*pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* Stack overflow hook — wired to the existing Deluge fault display. */
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName) {
	(void)xTask;
	(void)pcTaskName;
	extern void freezeWithError(const char* errmsg);
	freezeWithError("STAK");
}

/*
 * The main task function. Called with a function pointer to the scheduler
 * entry point (which calls registerTasks + startTaskManager). This avoids
 * the need for freertos_app.cpp to know about registerTasks() which is
 * file-local in deluge.cpp.
 */
static void mainTaskFunction(void* pvParameters) {
	void (*schedulerEntry)(void) = (void (*)(void))pvParameters;
	schedulerEntry(); /* This is an infinite loop — never returns */
}

extern "C" void startFreeRTOS(void (*schedulerEntry)(void)) {
	xTaskCreateStatic(mainTaskFunction, "MainScheduler", 8192,         /* Stack size in words (32KB) */
	                  (void*)schedulerEntry, configMAX_PRIORITIES - 1, /* Highest application priority */
	                  sMainTaskStack, &sMainTaskTCB);

	/* Start the FreeRTOS scheduler. This never returns. */
	vTaskStartScheduler();
}

#endif /* USE_FREERTOS */
