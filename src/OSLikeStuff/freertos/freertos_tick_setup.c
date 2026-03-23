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
 * FreeRTOS tick interrupt setup using MTU2 Timer 3.
 *
 * Timer 3 is the only unused MTU2 timer on the Deluge:
 *   Timer 0: TIMER_SYSTEM_SUPERFAST (USB)
 *   Timer 1: TIMER_SYSTEM_FAST (PIC/audio)
 *   Timer 2: TIMER_MIDI_GATE_OUTPUT
 *   Timer 3: ** Available — used here for FreeRTOS tick **
 *   Timer 4: TIMER_SYSTEM_SLOW (OLED/USB)
 *
 * P0 clock = 33.33 MHz, prescale = 64 → 520,833 Hz
 * TGRA compare = 521 → 520,833 / 521 ≈ 999.7 Hz (close to 1kHz target)
 */

#include "FreeRTOS.h"
#include "RZA1/intc/devdrv_intc.h"
#include "RZA1/mtu/mtu.h"
#include "task.h"

#define TICK_TIMER 3
#define TICK_PRESCALE 64
/* P0 = 33,330,000 Hz. With prescale 64: 33,330,000 / 64 = 520,781 Hz.
 * For a 1kHz tick: 520,781 / 1000 ≈ 521 counts. */
#define TICK_COMPARE ((uint16_t)(33330000UL / TICK_PRESCALE / configTICK_RATE_HZ))

static void freeRTOSTickISR(uint32_t intSense) {
	timerClearCompareMatchTGRA(TICK_TIMER);

	/* In Phase 1 with a single application task, we only need to increment
	 * the tick counter. No context switch is required because there is only
	 * one app task (plus idle, which never runs while the app task is active). */
	xTaskIncrementTick();
}

void vConfigureTickInterrupt(void) {
	/* Follow the same pattern as setupTimerWithInterruptHandler() in
	 * timers_interrupts.c — disable, clear, configure, register ISR. */
	disableTimer(TICK_TIMER);
	*TCNT[TICK_TIMER] = 0u;
	timerClearCompareMatchTGRA(TICK_TIMER);
	*TIER[TICK_TIMER] = 1;                           /* Enable TGIA interrupt */
	timerControlSetup(TICK_TIMER, 1, TICK_PRESCALE); /* Cleared by TGRA match */
	*TGRA[TICK_TIMER] = TICK_COMPARE;

	/* Register in the existing GIC interrupt handler table.
	 * INTC_ID_TGI3A = 154 (from devdrv_intc.h). Priority 20 keeps the tick
	 * below latency-sensitive ISRs (audio DMA, USB, MIDI). */
	R_INTC_RegistIntFunc(INTC_ID_TGIA[TICK_TIMER], freeRTOSTickISR);
	R_INTC_SetPriority(INTC_ID_TGIA[TICK_TIMER], 20);
	R_INTC_Enable(INTC_ID_TGIA[TICK_TIMER]);

	enableTimer(TICK_TIMER);
}

void vClearTickInterrupt(void) {
	timerClearCompareMatchTGRA(TICK_TIMER);
}
