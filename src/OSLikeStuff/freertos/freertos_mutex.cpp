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

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "freertos_mutex.h"

/* Compile-time check that our opaque storage is large enough. */
static_assert(sizeof(rtos_mutex_storage_t) >= sizeof(StaticSemaphore_t),
              "rtos_mutex_storage_t too small for StaticSemaphore_t");

/* During early boot (before vTaskStartScheduler), all code is single-threaded
 * so mutex operations are unnecessary. FreeRTOS semaphore APIs assert-fail if
 * called before the scheduler is running, so we no-op in that case. */
static inline bool schedulerRunning() {
	return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

extern "C" {

rtos_mutex_t rtos_mutex_create(rtos_mutex_storage_t* storage) {
	return xSemaphoreCreateMutexStatic((StaticSemaphore_t*)storage);
}

rtos_mutex_t rtos_mutex_create_recursive(rtos_mutex_storage_t* storage) {
	return xSemaphoreCreateRecursiveMutexStatic((StaticSemaphore_t*)storage);
}

void rtos_mutex_lock(rtos_mutex_t mutex) {
	if (!schedulerRunning()) {
		return;
	}
	xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

void rtos_mutex_unlock(rtos_mutex_t mutex) {
	if (!schedulerRunning()) {
		return;
	}
	xSemaphoreGive((SemaphoreHandle_t)mutex);
}

void rtos_mutex_lock_recursive(rtos_mutex_t mutex) {
	if (!schedulerRunning()) {
		return;
	}
	xSemaphoreTakeRecursive((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

void rtos_mutex_unlock_recursive(rtos_mutex_t mutex) {
	if (!schedulerRunning()) {
		return;
	}
	xSemaphoreGiveRecursive((SemaphoreHandle_t)mutex);
}

bool rtos_mutex_trylock(rtos_mutex_t mutex) {
	if (!schedulerRunning()) {
		return true; /* Single-threaded — always succeed */
	}
	return xSemaphoreTake((SemaphoreHandle_t)mutex, 0) == pdTRUE;
}

bool rtos_mutex_is_locked(rtos_mutex_t mutex) {
	if (!schedulerRunning()) {
		return false; /* Single-threaded — never contended */
	}
	/* Try to take it with zero timeout. If we get it, it wasn't locked —
	 * give it back immediately. If we don't get it, it's locked. */
	if (xSemaphoreTake((SemaphoreHandle_t)mutex, 0) == pdTRUE) {
		xSemaphoreGive((SemaphoreHandle_t)mutex);
		return false;
	}
	return true;
}

} /* extern "C" */

#endif /* USE_FREERTOS */
