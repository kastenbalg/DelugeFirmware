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

#include "storage/cluster/cluster_priority_queue.h"

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "task.h"

extern TaskHandle_t clusterLoaderTaskHandle;

void clusterQueueEnterCritical() {
	taskENTER_CRITICAL();
}

void clusterQueueExitCritical() {
	taskEXIT_CRITICAL();
}

void clusterQueueNotifyLoader() {
	if (clusterLoaderTaskHandle != nullptr) {
		xTaskNotifyGive(clusterLoaderTaskHandle);
	}
}

#else

void clusterQueueEnterCritical() {
}

void clusterQueueExitCritical() {
}

void clusterQueueNotifyLoader() {
}

#endif
