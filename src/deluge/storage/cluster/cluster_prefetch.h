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
 * Prefetch sample clusters for upcoming notes and arpeggiator note pools.
 *
 * Called from the sequencer task after processing ticks. Three tiers:
 *
 * Tier 1: Drain active voice hints — clusters needed within ~93ms by
 *         currently-playing voices crossing cluster boundaries.
 *
 * Tier 2: Scan upcoming note starts — scans each active InstrumentClip's
 *         NoteRows to find notes that will fire within the look-ahead
 *         window and enqueues their starting clusters.
 *
 * Tier 3: Arpeggiator note pool — enumerates all notes an active
 *         arpeggiator could generate (input notes × octave range × chord
 *         offsets), resolves them to Samples, and prefetches their starting
 *         clusters with priority based on distance from the arp's current
 *         position. This prevents note skipping when the arp generates
 *         notes that map to samples not yet loaded.
 *
 * @param lookAheadTicks  How far ahead to scan in internal tick units.
 *                        Should cover at least 2 cluster load times (~32ms).
 *                        Set to 0 for automatic calculation based on tempo.
 */
void prefetchUpcomingSampleClusters(int32_t lookAheadTicks = 0);

class Sample;

/**
 * Push a cluster prefetch hint from the audio task.
 * Called from SampleLowLevelReader::moveOnToNextCluster() when a voice
 * crosses a cluster boundary. The sequencer drains these hints and
 * prefetches clusters ahead of the voice's current position.
 *
 * Lock-free SPSC: audio task is sole producer, sequencer is sole consumer.
 * Aligned 32-bit reads/writes on ARM Cortex-A9 are naturally atomic.
 */
void clusterPrefetchHintPush(Sample* sample, int32_t clusterIndex, int8_t playDirection);
