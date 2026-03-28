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

#include "storage/cluster/cluster_prefetch.h"

#include "definitions_cxx.hpp"
#include "model/clip/clip.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/note/note_vector.h"
#include "model/sample/sample.h"
#include "model/sample/sample_cluster.h"
#include "model/sample/sample_holder.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "processing/source.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "storage/multi_range/multi_range.h"

extern Song* currentSong;

/* ========================================================================
 * Prefetch hint ring buffer: audio task → sequencer task.
 * Lock-free SPSC. Audio writes hints when voices cross cluster boundaries;
 * sequencer drains them and enqueues clusters ahead of active voice positions.
 * ======================================================================== */

struct ClusterPrefetchHint {
	Sample* sample;
	int32_t clusterIndex;
	int8_t playDirection; /* +1 forward, -1 reverse */
};

static constexpr int32_t kPrefetchHintSize = 16;
static ClusterPrefetchHint sPrefetchHints[kPrefetchHintSize];
static volatile uint32_t sPrefetchHintHead = 0; /* audio task writes */
static volatile uint32_t sPrefetchHintTail = 0; /* sequencer reads */

void clusterPrefetchHintPush(Sample* sample, int32_t clusterIndex, int8_t playDirection) {
	uint32_t next = (sPrefetchHintHead + 1) % kPrefetchHintSize;
	if (next == sPrefetchHintTail) {
		return; /* Ring full — drop (shouldn't happen at ~once per 93ms per voice) */
	}
	sPrefetchHints[sPrefetchHintHead].sample = sample;
	sPrefetchHints[sPrefetchHintHead].clusterIndex = clusterIndex;
	sPrefetchHints[sPrefetchHintHead].playDirection = playDirection;
	__asm__ volatile("dmb" ::: "memory");
	sPrefetchHintHead = next;
}

/**
 * Prefetch clusters by index (rather than byte position).
 * Enqueues `count` clusters starting from `startClusterIndex` in the given direction.
 */
static int32_t prefetchClustersFromIndex(Sample* sample, int32_t startClusterIndex, int8_t direction, int32_t count) {
	int32_t clustersEnqueued = 0;
	int32_t numClusters = sample->clusters.getNumElements();

	for (int32_t i = 0; i < count; i++) {
		int32_t idx = startClusterIndex + i * direction;
		if (idx < 0 || idx >= numClusters) {
			break;
		}

		SampleCluster* sc = sample->clusters.getElement(idx);
		if (sc == nullptr) {
			break;
		}

		if (sc->cluster == nullptr) {
			sc->getCluster(sample, idx, CLUSTER_ENQUEUE, 0);
			clustersEnqueued++;
		}
	}

	return clustersEnqueued;
}

/**
 * Drain the prefetch hint ring and enqueue clusters ahead of active voices.
 * This is "tier 1" prefetching — highest priority because these clusters
 * are needed within ~93ms (one cluster's worth of audio).
 */
static void drainPrefetchHints() {
	while (sPrefetchHintTail != sPrefetchHintHead) {
		__asm__ volatile("dmb" ::: "memory");
		const ClusterPrefetchHint& hint = sPrefetchHints[sPrefetchHintTail];

		/* moveOnToNextCluster() already enqueued hint.clusterIndex reactively.
		 * We prefetch BEYOND that — starting from clusterIndex + playDirection. */
		int32_t startCluster = hint.clusterIndex + hint.playDirection;
		prefetchClustersFromIndex(hint.sample, startCluster, hint.playDirection, 2);

		sPrefetchHintTail = (sPrefetchHintTail + 1) % kPrefetchHintSize;
	}
}

/* Default look-ahead for upcoming note prefetch.
 * Internal tick resolution is 24 ticks/QN scaled by magnitude (default mag 2 → 96 ticks/QN).
 * At 120 BPM: 64 ticks ≈ 333ms — ample time to load a cluster before a note fires.
 * Active voice positions are handled separately via the prefetch hint ring. */
static constexpr int32_t kDefaultLookAheadTicks = 64;

/* Maximum NoteRows to scan per call to limit sequencer CPU usage. */
static constexpr int32_t kMaxNoteRowsPerScan = 16;

/* Maximum clusters to enqueue per call. */
static constexpr int32_t kMaxClustersPerScan = 8;

/**
 * Given a Sample and a byte position within it, enqueue the cluster at that
 * position plus a few ahead. Returns the number of clusters enqueued.
 */
static int32_t prefetchClustersForSample(Sample* sample, int32_t startByte, int32_t maxToEnqueue) {
	int32_t clustersEnqueued = 0;
	int32_t clusterIndex = startByte >> Cluster::size_magnitude;
	int32_t numClusters = sample->clusters.getNumElements();

	/* Enqueue the starting cluster and a couple ahead */
	for (int32_t i = 0; i < kNumClustersLoadedAhead && clustersEnqueued < maxToEnqueue; i++) {
		if (clusterIndex < 0 || clusterIndex >= numClusters) {
			break;
		}

		SampleCluster* sc = sample->clusters.getElement(clusterIndex);
		if (sc == nullptr) {
			break;
		}

		/* Only enqueue if not already loaded */
		if (sc->cluster == nullptr) {
			/* getCluster with CLUSTER_ENQUEUE creates the Cluster object and
			 * adds it to the loading queue. Priority 0 = highest priority. */
			sc->getCluster(sample, clusterIndex, CLUSTER_ENQUEUE, 0);
			clustersEnqueued++;
		}

		clusterIndex++;
	}

	return clustersEnqueued;
}

/**
 * For a given Sound source, resolve note code to Sample and prefetch clusters.
 * Returns the number of clusters enqueued.
 */
static int32_t prefetchForSource(Source& source, int32_t noteCode, int32_t transpose, int32_t maxToEnqueue) {
	if (source.oscType != OscType::SAMPLE) {
		return 0;
	}

	MultiRange* range = source.getRange(noteCode + transpose);
	if (range == nullptr) {
		return 0;
	}

	AudioFileHolder* holder = range->getAudioFileHolder();
	if (holder == nullptr || holder->audioFile == nullptr) {
		return 0;
	}

	Sample* sample = static_cast<Sample*>(holder->audioFile);
	if (sample->unloadable) {
		return 0;
	}

	/* Calculate the starting byte position. For a simple prefetch, use the
	 * sample's default start position (where playback begins for this range). */
	SampleHolder* sampleHolder = static_cast<SampleHolder*>(holder);
	int32_t bytesPerSample = sample->numChannels * sample->byteDepth;
	int32_t startSample = (int64_t)sampleHolder->startPos;
	if (startSample < 0) {
		startSample = 0;
	}
	int32_t startByte = sample->audioDataStartPosBytes + startSample * bytesPerSample;

	return prefetchClustersForSample(sample, startByte, maxToEnqueue);
}

void prefetchUpcomingSampleClusters(int32_t lookAheadTicks) {
	/* Tier 1: Drain active voice hints (highest priority).
	 * These represent clusters needed within ~93ms by currently-playing voices. */
	drainPrefetchHints();

	if (currentSong == nullptr) {
		return;
	}

	if (!playbackHandler.isEitherClockActive()) {
		return;
	}

	if (lookAheadTicks <= 0) {
		lookAheadTicks = kDefaultLookAheadTicks;
	}

	/* Tier 2: Scan upcoming note starts for initial cluster prefetch. */
	int32_t clustersEnqueued = 0;
	int32_t noteRowsScanned = 0;

	/* Iterate active clips */
	int32_t numClips = currentSong->sessionClips.getNumElements();
	for (int32_t c = 0; c < numClips && clustersEnqueued < kMaxClustersPerScan; c++) {
		Clip* clip = currentSong->sessionClips.getClipAtIndex(c);

		if (clip == nullptr || !clip->activeIfNoSolo) {
			continue;
		}

		/* Audio clips stream linearly — the audio engine's SampleLowLevelReader
		 * already maintains a 2-cluster sliding window. No prefetch needed.
		 * Only InstrumentClips with sample-based synths need prefetching,
		 * because new notes create unpredictable cluster demands. */
		if (clip->type != ClipType::INSTRUMENT) {
			continue;
		}

		InstrumentClip* iclip = static_cast<InstrumentClip*>(clip);
		Output* output = iclip->output;
		if (output == nullptr || (output->type != OutputType::SYNTH && output->type != OutputType::KIT)) {
			continue;
		}

		bool isKit = (output->type == OutputType::KIT);

		/* For Synth: cast to Sound for source access.
		 * For Kit: each NoteRow has its own SoundDrum with sources. */
		Sound* clipSound = nullptr;
		if (!isKit) {
			SoundInstrument* soundInst = static_cast<SoundInstrument*>(output);
			clipSound = static_cast<Sound*>(soundInst);
		}

		/* Current playback position in this clip */
		int32_t currentPos = iclip->lastProcessedPos;
		int32_t clipLength = iclip->loopLength;
		int32_t searchEnd = currentPos + lookAheadTicks;

		/* Scan each NoteRow for upcoming notes */
		int32_t numNoteRows = iclip->noteRows.getNumElements();
		for (int32_t r = 0;
		     r < numNoteRows && noteRowsScanned < kMaxNoteRowsPerScan && clustersEnqueued < kMaxClustersPerScan; r++) {

			NoteRow* noteRow = iclip->noteRows.getElement(r);
			if (noteRow == nullptr || noteRow->muted) {
				continue;
			}
			if (noteRow->notes.getNumElements() == 0) {
				continue;
			}

			/* For Kit: get the Sound from the NoteRow's drum */
			Sound* sound = clipSound;
			if (isKit) {
				Drum* drum = noteRow->drum;
				if (drum == nullptr || drum->type != DrumType::SOUND) {
					continue;
				}
				sound = static_cast<SoundDrum*>(drum);
			}
			if (sound == nullptr) {
				continue;
			}

			noteRowsScanned++;

			/* Binary search for the next note at or after currentPos */
			int32_t searchPos = currentPos;

			int32_t noteIndex = noteRow->notes.search(searchPos, GREATER_OR_EQUAL);
			int32_t numNotes = noteRow->notes.getNumElements();

			/* Check the next few notes within our look-ahead window */
			for (int32_t attempt = 0; attempt < 3 && clustersEnqueued < kMaxClustersPerScan; attempt++) {
				if (noteIndex >= numNotes) {
					/* Wrap around to beginning of clip */
					noteIndex = 0;
					searchPos = 0;
				}
				if (noteIndex >= numNotes) {
					break; /* Empty note list */
				}

				Note* note = noteRow->notes.getElement(noteIndex);
				if (note == nullptr) {
					break;
				}

				/* Check if this note is within our look-ahead window. */
				int32_t notePos = note->pos;
				int32_t distance = notePos - currentPos;
				if (distance < 0) {
					distance += clipLength; /* Wrapped */
				}

				if (distance > lookAheadTicks) {
					break; /* Beyond look-ahead, stop searching this NoteRow */
				}

				/* This note will fire soon — prefetch its sample clusters.
				 * For Synth: resolve through Sound's sources.
				 * For Kit: each NoteRow's SoundDrum has its own sources. */
				int32_t noteCode = noteRow->getNoteCode();

				for (int32_t s = 0; s < kNumSources && clustersEnqueued < kMaxClustersPerScan; s++) {
					clustersEnqueued += prefetchForSource(sound->sources[s], noteCode, sound->transpose,
					                                      kMaxClustersPerScan - clustersEnqueued);
				}

				noteIndex++;
			}
		}
	}
}
