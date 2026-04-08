/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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

#include "model/clip/instrument_clip.h"
#include "model/scale/musical_key.h"
#include "model/scale/note_set.h"

class Action;
class ModelStackWithNoteRow;
class ModelStackWithTimelineCounter;
class NoteRow;
class Song;

struct ScaleChange;

/// Concrete InstrumentClip subclass for pitch-based instruments (Synth, MIDI, CV).
///
/// Melodic clips use scale-aware NoteRows keyed by pitch (yNote), and a global
/// ParamManager.  This is fundamentally different from the drum-indexed layout
/// used by Kit clips (see KitClip).
class MelodicClip final : public InstrumentClip {
public:
	using InstrumentClip::InstrumentClip;

	// --- Identity ---
	bool isKitClip() const override { return false; }
	bool isConcreteSubclass() const override { return true; }
	// getXMLTag() intentionally returns "instrumentClip" for backward compatibility (inherited default)

	// --- Y-coordinate dispatch (pitch-based with scale transformation) ---
	NoteRow* getNoteRowOnScreen(int32_t yDisplay, Song* song, int32_t* getIndex = nullptr) override;
	ModelStackWithNoteRow* getNoteRowOnScreen(int32_t yDisplay, ModelStackWithTimelineCounter* modelStack) override;
	int32_t getYVisualFromYNote(int32_t yNote, Song* song) override;
	int32_t getYNoteFromYVisual(int32_t yVisual, Song* song) override;

	// --- Note tail dispatch (instrument-wide: MIDI/CV always true, SYNTH via instrument) ---
	bool allowNoteTails(ModelStackWithNoteRow* modelStack) override;

	// --- Melodic-specific methods (moved from InstrumentClip in Phase 1.3) ---
	void transpose(int32_t semitones, ModelStackWithTimelineCounter* modelStack);
	void nudgeNotesVertically(int32_t direction, VerticalNudgeType type, ModelStackWithTimelineCounter* modelStack);
	void replaceMusicalMode(const ScaleChange& changes, ModelStackWithTimelineCounter* modelStack);
	void noteRemovedFromMode(int32_t yNoteWithinOctave, Song* song);
	void seeWhatNotesWithinOctaveArePresent(NoteSet& notesWithinOctavePresent, MusicalKey key);
	ModelStackWithNoteRow* getOrCreateNoteRowForYNote(int32_t yNote, ModelStackWithTimelineCounter* modelStack,
	                                                  Action* action = nullptr, bool* scaleAltered = nullptr);
	NoteRow* createNewNoteRowForYVisual(int32_t yVisual, Song* song);
	int16_t getTopYNote();
	int16_t getBottomYNote();
	bool isScaleModeClip();
};
