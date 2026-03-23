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

#ifndef FREERTOS_MUTEX_H
#define FREERTOS_MUTEX_H

/*
 * Thin C-linkage wrapper around FreeRTOS mutexes for use by both C and C++ code.
 * Under non-FreeRTOS builds, falls back to boolean flags (safe in cooperative scheduling).
 * All mutexes use static allocation — no FreeRTOS heap required.
 *
 * The header uses opaque types to avoid pulling FreeRTOS headers into every
 * translation unit. The actual FreeRTOS types are only needed in freertos_mutex.cpp.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef USE_FREERTOS

/* Opaque handle — matches FreeRTOS SemaphoreHandle_t (void*) */
typedef void* rtos_mutex_t;

/* Static storage sized to hold a StaticSemaphore_t.
 * StaticSemaphore_t is 80 bytes on Cortex-A9 FreeRTOS V11; we round up. */
typedef struct {
	uint8_t opaque[88];
} rtos_mutex_storage_t;

/* Create a mutex using caller-provided static storage. Returns the handle. */
rtos_mutex_t rtos_mutex_create(rtos_mutex_storage_t* storage);

/* Block until the mutex is acquired.
 * Uses priority inheritance to prevent unbounded inversion. */
void rtos_mutex_lock(rtos_mutex_t mutex);

/* Release the mutex. */
void rtos_mutex_unlock(rtos_mutex_t mutex);

/* Try to acquire the mutex without blocking. Returns true if acquired. */
bool rtos_mutex_trylock(rtos_mutex_t mutex);

/* Check if the mutex is currently held (without acquiring it).
 * Used by ResourceChecker for non-blocking availability queries. */
bool rtos_mutex_is_locked(rtos_mutex_t mutex);

#else /* !USE_FREERTOS — boolean fallback for cooperative scheduling */

typedef volatile bool* rtos_mutex_t;
typedef volatile bool rtos_mutex_storage_t;

static inline rtos_mutex_t rtos_mutex_create(rtos_mutex_storage_t* storage) {
	*storage = false;
	return storage;
}

static inline void rtos_mutex_lock(rtos_mutex_t mutex) {
	*mutex = true;
}

static inline void rtos_mutex_unlock(rtos_mutex_t mutex) {
	*mutex = false;
}

static inline bool rtos_mutex_trylock(rtos_mutex_t mutex) {
	if (*mutex) {
		return false;
	}
	*mutex = true;
	return true;
}

static inline bool rtos_mutex_is_locked(rtos_mutex_t mutex) {
	return *mutex;
}

#endif /* USE_FREERTOS */

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_MUTEX_H */
