/*
 * Copyright © 2025 Mark Adams
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
#ifndef RESOURCE_CHECKER_H
#define RESOURCE_CHECKER_H
#include <OSLikeStuff/freertos/freertos_mutex.h>
#include <OSLikeStuff/scheduler_api.h>
#include <bitset>
#include <extern.h>
extern uint8_t currentlyAccessingCard;
extern uint32_t usbLock;
extern rtos_mutex_t sdCardMutex;
extern rtos_mutex_t usbMutex;
// this is basically a bitset however the enums need to be exposed to C code and this is easier to keep synced
class ResourceChecker {
	uint32_t resources_{0};

public:
	ResourceChecker() = default;
	ResourceChecker(ResourceID resources) : resources_(resources) {};
	/// returns whether all resources are available. This is basically shitty priority ceiling to avoid the potential of
	/// a task locking one resource and then trying to yield while it waits for another.
	/// Checks both FreeRTOS mutexes and legacy booleans — some locks (e.g. usbLock in ISR context) can only use
	/// booleans, while cross-task locks use mutexes.
	bool checkResources() const {
		if (resources_ == RESOURCE_NONE) {
			return true;
		}
		bool anythingLocked = false;
		if ((resources_ & RESOURCE_SD) != 0u) {
			anythingLocked |= currentlyAccessingCard;
			anythingLocked |= rtos_mutex_is_locked(sdCardMutex);
		}
		if ((resources_ & RESOURCE_USB) != 0u) {
			anythingLocked |= (bool)usbLock;
			anythingLocked |= rtos_mutex_is_locked(usbMutex);
		}
		if ((resources_ & RESOURCE_SD_ROUTINE) != 0u) {
			anythingLocked |= sdRoutineLock;
			anythingLocked |= rtos_mutex_is_locked(sdRoutineMutex);
		}
		return !anythingLocked;
	}
};

#endif // RESOURCE_CHECKER_H
