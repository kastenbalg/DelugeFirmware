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
 * FreeRTOS application integration for the Deluge firmware.
 *
 * Phase 4 architecture:
 * - Audio task at FreeRTOS priority 7 (highest) — DSP only, never touches SD
 * - Cluster loader task at priority 5 — loads sample clusters from SD card
 * - App task at FreeRTOS priority 3 — runs cooperative scheduler for UI/file ops
 * - IRQ handler bridges to existing GIC dispatch via vApplicationFPUSafeIRQHandler
 */

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* --------------------------------------------------------------------------
 * IRQ bridge: FreeRTOS IRQ handler → existing Deluge GIC dispatch
 *
 * FreeRTOS's FreeRTOS_IRQ_Handler (portASM.S) reads ICCIAR, saves context,
 * calls vApplicationFPUSafeIRQHandler(icciar), writes ICCEOIR, then checks
 * ulPortYieldRequired for context switch. We bridge to the existing Deluge
 * interrupt dispatch which handles all registered ISRs.
 * -------------------------------------------------------------------------- */
extern "C" {
void INTC_Handler_Interrupt(uint32_t icciar);

void vApplicationFPUSafeIRQHandler(uint32_t ulICCIAR) {
	INTC_Handler_Interrupt(ulICCIAR);
}
}

/* --------------------------------------------------------------------------
 * Static task allocations
 * -------------------------------------------------------------------------- */

/* App task: runs cooperative scheduler for all non-audio tasks. 32KB stack. */
static StaticTask_t sAppTaskTCB;
static StackType_t sAppTaskStack[8192];

/* Audio task: dedicated high-priority task. 16KB stack. */
static StaticTask_t sAudioTaskTCB;
static StackType_t sAudioTaskStack[4096];

/* Cluster loader task: loads sample clusters from SD card. 8KB stack. */
static StaticTask_t sClusterLoaderTCB;
static StackType_t sClusterLoaderStack[2048];

/* Global handle for task notification from ClusterPriorityQueue::enqueueCluster() */
TaskHandle_t clusterLoaderTaskHandle = nullptr;

/* Idle task (required when configSUPPORT_STATIC_ALLOCATION=1). */
static StaticTask_t sIdleTaskTCB;
static StackType_t sIdleTaskStack[configMINIMAL_STACK_SIZE];

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t** ppxIdleTaskTCBBuffer, StackType_t** ppxIdleTaskStackBuffer,
                                              uint32_t* pulIdleTaskStackSize) {
	*ppxIdleTaskTCBBuffer = &sIdleTaskTCB;
	*ppxIdleTaskStackBuffer = sIdleTaskStack;
	*pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName) {
	(void)xTask;
	(void)pcTaskName;
	extern void freezeWithError(const char* errmsg);
	freezeWithError("STAK");
}

/* --------------------------------------------------------------------------
 * Audio task: highest priority, runs AudioEngine::routine() in a loop
 * -------------------------------------------------------------------------- */
namespace AudioEngine {
void routine();
}

static void audioTaskFunction(void* pvParameters) {
	(void)pvParameters;
	TickType_t xLastWakeTime = xTaskGetTickCount();
	for (;;) {
		AudioEngine::routine();
		/* Wake every ~1ms. Audio polls DMA buffer position inside routine()
		 * to determine how many samples to render. With 1kHz tick this wakes
		 * roughly every 1.45ms worth of audio (64 samples @ 44.1kHz). */
		vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1));
	}
}

/* --------------------------------------------------------------------------
 * Cluster loader task: dedicated task for loading sample clusters from SD
 * -------------------------------------------------------------------------- */
#include "storage/audio/audio_file_manager.h"

static void clusterLoaderTaskFunction(void* pvParameters) {
	(void)pvParameters;
	audioFileManager.clusterLoaderMain(); /* Infinite loop — never returns */
}

/* --------------------------------------------------------------------------
 * App task: runs existing cooperative scheduler for non-audio tasks
 * -------------------------------------------------------------------------- */
static void appTaskFunction(void* pvParameters) {
	void (*schedulerEntry)(void) = (void (*)(void))pvParameters;
	schedulerEntry(); /* Infinite loop — never returns */
}

/* --------------------------------------------------------------------------
 * Entry point: create tasks and start the FreeRTOS scheduler
 * -------------------------------------------------------------------------- */
extern "C" void startFreeRTOS(void (*schedulerEntry)(void)) {
	/* App task at priority 3 — all non-audio tasks run cooperatively here */
	xTaskCreateStatic(appTaskFunction, "App", 8192, (void*)schedulerEntry,
	                  3, /* Same priority for all non-audio, no time-slicing */
	                  sAppTaskStack, &sAppTaskTCB);

	/* Cluster loader task at priority 5 — above app, below audio */
	clusterLoaderTaskHandle = xTaskCreateStatic(clusterLoaderTaskFunction, "ClusterLoader", 2048, NULL, 5,
	                                            sClusterLoaderStack, &sClusterLoaderTCB);

	/* Audio task at highest priority — preempts everything */
	xTaskCreateStatic(audioTaskFunction, "Audio", 4096, NULL, configMAX_PRIORITIES - 1, /* Priority 7 */
	                  sAudioTaskStack, &sAudioTaskTCB);

	/* Start the FreeRTOS scheduler. This never returns. */
	vTaskStartScheduler();
}

#endif /* USE_FREERTOS */
