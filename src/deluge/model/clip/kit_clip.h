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

class Drum;
class ModelStackWithTimelineCounter;
class ModelStackWithNoteRow;
class NoteRow;
class Song;

/// Concrete InstrumentClip subclass for Kit instruments.
///
/// Kit clips use drum-indexed NoteRows (array index = yDisplay + yScroll) and
/// per-drum ParamManagers.  This is fundamentally different from the pitch-based
/// layout used by Synth/MIDI/CV clips (see MelodicClip).
class KitClip final : public InstrumentClip {
public:
	using InstrumentClip::InstrumentClip;

	// --- Identity ---
	bool isKitClip() const override { return true; }
	bool isConcreteSubclass() const override { return true; }
	char const* getXMLTag() override { return "kitClip"; }

	// --- Y-coordinate dispatch (array-index based for kits) ---
	NoteRow* getNoteRowOnScreen(int32_t yDisplay, Song* song, int32_t* getIndex = nullptr) override;
	ModelStackWithNoteRow* getNoteRowOnScreen(int32_t yDisplay, ModelStackWithTimelineCounter* modelStack) override;
	int32_t getYVisualFromYNote(int32_t yNote, Song* song) override { return yNote; }
	int32_t getYNoteFromYVisual(int32_t yVisual, Song* song) override { return yVisual; }

	// --- Note tail dispatch (per-drum check) ---
	bool allowNoteTails(ModelStackWithNoteRow* modelStack) override;

	// --- ModControllable/ParamManager derivation ---
	void getActiveModControllable(ModelStackWithTimelineCounter* modelStack) override;
	ModControllable* getModControllableForNoteRow(NoteRow* noteRow) override;
	ParamManager* getParamManagerForNoteRow(NoteRow* noteRow) override;

	// --- Kit-specific methods (moved from InstrumentClip in Phase 1.3) ---
	NoteRow* createNewNoteRowForKit(ModelStackWithTimelineCounter* modelStack, bool atStart,
	                                int32_t* getIndex = nullptr);
	void assignDrumsToNoteRows(ModelStackWithTimelineCounter* modelStack, bool shouldGiveMIDICommandsToDrums = false,
	                           int32_t numNoteRowsPreviouslyDeletedFromBottom = 0);
	void unassignAllNoteRowsFromDrums(ModelStackWithTimelineCounter* modelStack, bool shouldRememberDrumNames,
	                                  bool shouldRetainLinksToSounds, bool shouldGrabMidiCommands,
	                                  bool shouldBackUpExpressionParamsToo);
	ModelStackWithNoteRow* getNoteRowForSelectedDrum(ModelStackWithTimelineCounter* modelStack);
	ModelStackWithNoteRow* getNoteRowForDrum(ModelStackWithTimelineCounter* modelStack, Drum* drum);
	NoteRow* getNoteRowForDrum(Drum* drum, int32_t* getIndex = nullptr);
	ModelStackWithNoteRow* getNoteRowForDrumName(ModelStackWithTimelineCounter* modelStack, char const* name);
	void prepNoteRowsForExitingKitMode(Song* song);
	void setupAsNewKitClipIfNecessary(ModelStackWithTimelineCounter* modelStack);
	void ensureScrollWithinKitBounds();
	void deleteOldDrumNames();

private:
	void prepareToEnterKitMode(Song* song);
};
