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

#pragma once

#include <cstdint>

/**
 * Prefetch sample clusters for upcoming sequenced notes.
 *
 * Called from the sequencer task after processing ticks. Scans each active
 * InstrumentClip's NoteRows to find notes that will fire within the
 * look-ahead window, resolves them to Samples, and enqueues their starting
 * clusters for loading.
 *
 * This ensures clusters are loaded BEFORE the audio task needs them,
 * eliminating the "-  F" (file not found) errors that occur when the
 * audio task discovers an unloaded cluster mid-render.
 *
 * @param lookAheadTicks  How far ahead to scan in internal tick units.
 *                        Should cover at least 2 cluster load times (~32ms).
 *                        Set to 0 for automatic calculation based on tempo.
 */
void prefetchUpcomingSampleClusters(int32_t lookAheadTicks = 0);
