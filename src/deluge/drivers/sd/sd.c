/*
 * Copyright © 2020-2023 Synthstrom Audible Limited
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

// Most of the contents of this file have been copied from Renesas's SD card driver libraries.

#include "definitions.h"
#include "drivers/mtu/mtu.h"

#include "RZA1/sdhi/inc/sdif.h"
#include "deluge.h"
#include "scheduler_api.h"
#include "util/cfunctions.h"

#ifdef USE_FREERTOS
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* Declared in sd_dev_low.c */
extern SemaphoreHandle_t sSdhiSemaphore;
#endif

uint16_t stopTime;

bool wrappedCheckTimer() {
	// this is a bit odd but it returns err when the timer goes off
	return (sddev_check_timer() == SD_ERR);
}

/******************************************************************************
 * Function Name: int32_t sddev_power_on(int32_t sd_port);
 * Description  : Power-on H/W to use SDHI
 * Arguments    : none
 * Return Value : success : SD_OK
 *              : fail    : SD_ERR
 ******************************************************************************/
int32_t sddev_power_on(int32_t sd_port) {
	/* ---Power On SD ---- */

	/* ---- Wait for  SD Wake up ---- */
	sddev_start_timer(100); /* wait 100ms */
#ifdef USE_TASK_MANAGER
	yieldingRoutineForSD(wrappedCheckTimer);
#else
	while (sddev_check_timer() == SD_OK) {
		/* wait */
		routineForSD(); // By Rohan
	}
#endif
	sddev_end_timer();

	return SD_OK;
}

bool sdIntFinished() {
	return (sd_check_int(SD_PORT) == SD_OK);
}
/******************************************************************************
 * Function Name: int32_t sddev_int_wait(int32_t sd_port, int32_t time);
 * Description  : Waitting for SDHI Interrupt
 * Arguments    : int32_t time : time out value to wait interrupt
 * Return Value : get interrupt : SD_OK
 *              : time out      : SD_ERR
 ******************************************************************************/
int32_t sddev_int_wait(int32_t sd_port, int32_t time) {

	logAudioAction("sddev_int_wait");

#ifdef USE_FREERTOS
	/* Under FreeRTOS, block on the SDHI semaphore instead of polling.
	 * The ISR (sddev_sd_int_handler_0/1) gives this semaphore when
	 * the SDHI interrupt fires. The calling task sleeps, freeing the
	 * CPU for audio, timer daemon, and other tasks.
	 *
	 * Before the scheduler is running (boot), fall through to the
	 * polling path below. */
	if (sSdhiSemaphore != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		/* Check if the interrupt already fired before we got here */
		if (sd_check_int(sd_port) == SD_OK) {
			return SD_OK;
		}

		/* Block on semaphore. The ISR (sddev_sd_int_handler_0/1) gives
		 * this semaphore when any SDHI interrupt fires. Since multiple
		 * interrupts occur per sd_read_sect (command response, data
		 * transfer, etc.), the semaphore may fire for a different event
		 * than the one we're waiting for. In that case, sd_check_int
		 * returns SD_ERR and we retry the semaphore. */
		TickType_t ticks = pdMS_TO_TICKS(time);
		if (ticks == 0) {
			ticks = 1;
		}
		TickType_t deadline = xTaskGetTickCount() + ticks;

		while (1) {
			TickType_t remaining = deadline - xTaskGetTickCount();
			if ((int32_t)remaining <= 0) {
				break;
			}
			if (xSemaphoreTake(sSdhiSemaphore, remaining) == pdTRUE) {
				/* Semaphore given — check if OUR expected flags are set */
				if (sd_check_int(sd_port) == SD_OK) {
					return SD_OK;
				}
				/* Wrong event — retry semaphore for the real one */
				continue;
			}
			/* Semaphore timed out */
			break;
		}

		/* Final check in case interrupt fired during timeout logic */
		if (sd_check_int(sd_port) == SD_OK) {
			return SD_OK;
		}
		return SD_ERR;
	}
#endif

#ifdef USE_TASK_MANAGER
	if (yieldingRoutineWithTimeoutForSD(sdIntFinished, time / 1000.)) {
		return SD_OK;
	}
	else
		return SD_ERR;
#else
	int32_t loop;
	if (time > 500) {
		loop = (time / 500);
		if ((time % 500) != 0) {
			loop++;
		}
		time = 500;
	}
	else {
		loop = 1;
	}

	do {
		sddev_start_timer(time);

		while (1) {
			if (sd_check_int(sd_port) == SD_OK) {
				sddev_end_timer();
				return SD_OK;
			}
			if (sddev_check_timer() == SD_ERR) {
				break;
			}
			routineForSD();
		}

		loop--;
		if (loop <= 0) {
			break;
		}

	} while (1);

	sddev_end_timer();

	return SD_ERR;
#endif
}

/******************************************************************************
 * Function Name: static void sddev_start_timer(int32_t msec);
 * Description  : start timer
 * Arguments    :
 * Return Value : none
 ******************************************************************************/
void sddev_start_timer(int msec) {
	stopTime = *TCNT[TIMER_SYSTEM_SLOW] + msToSlowTimerCount(msec);
}

/******************************************************************************
 * Function Name: static void sddev_end_timer(void);
 * Description  : end timer
 * Arguments    :
 * Return Value : none
 ******************************************************************************/
void sddev_end_timer(void) {
}

/******************************************************************************
 * Function Name: static int32_t sddev_check_timer(void);
 * Description  : check
 * Arguments    :
 * Return Value : t
 ******************************************************************************/
int sddev_check_timer(void) {

	uint16_t howFarAbove = *TCNT[TIMER_SYSTEM_SLOW] - stopTime;

	if (howFarAbove < 16384) {
		return SD_ERR;
	}

	return SD_OK;
}
