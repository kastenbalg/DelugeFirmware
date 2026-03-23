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

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * Application specific definitions.
 *
 * FreeRTOS configuration for Synthstrom Deluge (RZ/A1H, Cortex-A9 @ 400MHz).
 *
 * Phase 2+3: Audio task at highest priority in its own FreeRTOS task.
 * All non-audio tasks run cooperatively in a single app task.
 * Resource locks use FreeRTOS mutexes.
 *----------------------------------------------------------*/

/* Cortex-A9 @ 400 MHz */
#define configCPU_CLOCK_HZ (400000000UL)

/* 1ms tick. Audio task polls DMA buffer position directly and does not
 * depend on tick timing, so 1kHz is sufficient. */
#define configTICK_RATE_HZ ((TickType_t)1000)

/* Scheduler */
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 0
#define configMAX_PRIORITIES 8
#define configIDLE_SHOULD_YIELD 1

/* Memory: static allocation only to avoid conflict with the existing
 * GeneralMemoryAllocator. No FreeRTOS heap is used. */
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configMINIMAL_STACK_SIZE ((uint32_t)256)
#define configTOTAL_HEAP_SIZE 0

/* Features */
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 0
#define configUSE_QUEUE_SETS 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_TIMERS 0
#define configUSE_CO_ROUTINES 0
#define configUSE_TRACE_FACILITY 0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0

/* Hooks */
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_MALLOC_FAILED_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW 2

/* ARM GIC configuration for RZ/A1H.
 * Distributor base: 0xE8201000 (ICDIPR0 at +0x400)
 * CPU interface:    0xE8202000 (ICCIAR at +0xC, ICCEOIR at +0x10) */
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS 0xE8201000UL
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET 0x1000UL
#define configUNIQUE_INTERRUPT_PRIORITIES 32

/* Interrupts with priority above this value (i.e. numerically lower) are not
 * managed by FreeRTOS and can nest freely. This keeps audio DMA, USB, and
 * other latency-sensitive ISRs outside FreeRTOS's critical sections.
 * Priorities 0-17 are above FreeRTOS; 18-31 are managed. */
#define configMAX_API_CALL_INTERRUPT_PRIORITY 18

/* Tick interrupt: MTU2 Timer 3 (the only unused timer).
 * Setup and clear functions are implemented in freertos_tick_setup.c */
extern void vConfigureTickInterrupt(void);
extern void vClearTickInterrupt(void);
#define configSETUP_TICK_INTERRUPT() vConfigureTickInterrupt()
#define configCLEAR_TICK_INTERRUPT() vClearTickInterrupt()

/* Assert: use the existing Deluge freeze-with-error mechanism */
extern void freezeWithError(const char* errmsg);
#define configASSERT(x)                                                                                                \
	if (!(x)) {                                                                                                        \
		freezeWithError("RTOS");                                                                                       \
	}

/* Tick counter: 32-bit (required by FreeRTOS V11+) */
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS

/* Cortex-A port specific */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0

/* All tasks get FPU/NEON context automatically — audio rendering uses
 * hardware floating-point and NEON SIMD extensively. */
#define configUSE_TASK_FPU_SUPPORT 2

/* Include standard API functions */
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_vTaskDelete 0
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskPrioritySet 0
#define INCLUDE_uxTaskPriorityGet 0
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetSchedulerState 1

#endif /* FREERTOS_CONFIG_H */
