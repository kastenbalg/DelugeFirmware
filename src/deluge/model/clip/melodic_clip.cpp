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

#include "model/clip/melodic_clip.h"
#include "memory/memory_allocator_interface.h"
#include "model/action/action.h"
#include "model/consequence/consequence_scale_add_note.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/scale/musical_key.h"
#include "model/scale/note_set.h"
#include "model/scale/scale_change.h"
#include "model/song/song.h"
#include "processing/sound/sound_instrument.h"
#include <cmath>
#include <new>

// -----------------------------------------------------------------------
// Y-coordinate virtual overrides
// -----------------------------------------------------------------------

NoteRow* MelodicClip::getNoteRowOnScreen(int32_t yDisplay, Song* song, int32_t* getIndex) {
	int32_t yNote = getYNoteFromYDisplay(yDisplay, song);
	return getNoteRowForYNote(yNote, getIndex);
}

ModelStackWithNoteRow* MelodicClip::getNoteRowOnScreen(int32_t yDisplay, ModelStackWithTimelineCounter* modelStack) {
	int32_t noteRowIndex;
	NoteRow* noteRow = getNoteRowOnScreen(yDisplay, modelStack->song, &noteRowIndex);
	int32_t noteRowId = 0;
	if (noteRow) {
		noteRowId = getNoteRowId(noteRow, noteRowIndex);
	}
	return modelStack->addNoteRow(noteRowId, noteRow);
}

int32_t MelodicClip::getYVisualFromYNote(int32_t yNote, Song* song) {
	return song->getYVisualFromYNote(yNote, inScaleMode);
}

int32_t MelodicClip::getYNoteFromYVisual(int32_t yVisual, Song* song) {
	return song->getYNoteFromYVisual(yVisual, inScaleMode);
}

// -----------------------------------------------------------------------
// Note tail virtual override
// -----------------------------------------------------------------------

bool MelodicClip::allowNoteTails(ModelStackWithNoteRow* modelStack) {
	if (output->type == OutputType::MIDI_OUT || output->type == OutputType::CV) {
		return true;
	}

	// SYNTH
	ModelStackWithSoundFlags* modelStackWithSoundFlags =
	    modelStack->addOtherTwoThings((SoundInstrument*)output, &paramManager)->addSoundFlags();
	return ((SoundInstrument*)output)->allowNoteTails(modelStackWithSoundFlags);
}

// -----------------------------------------------------------------------
// ModControllable/ParamManager derivation
// -----------------------------------------------------------------------

ModControllable* MelodicClip::getModControllableForNoteRow(NoteRow* /*noteRow*/) {
	return output ? output->toModControllable() : nullptr;
}

ParamManager* MelodicClip::getParamManagerForNoteRow(NoteRow* /*noteRow*/) {
	return &paramManager;
}

// -----------------------------------------------------------------------
// Melodic-specific methods (moved from InstrumentClip in Phase 1.3)
// -----------------------------------------------------------------------

void MelodicClip::transpose(int32_t semitones, ModelStackWithTimelineCounter* modelStack) {
	// Make sure no notes sounding
	stopAllNotesPlaying(modelStack);

	// Must also do auditioned notes, since transpose can now be sequenced and change
	// noterows while we hold an audition pad.
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStackWithSong = setupModelStackWithSong(modelStackMemory, currentSong);
	output->stopAnyAuditioning(modelStackWithSong);

	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		thisNoteRow->y += semitones;
	}

	yScroll += semitones;
	colourOffset -= semitones;
}

void MelodicClip::nudgeNotesVertically(int32_t direction, VerticalNudgeType type,
                                       ModelStackWithTimelineCounter* modelStack) {
	if (!direction) {
		return;
	}

	int32_t change = direction > 0 ? 1 : -1;
	if (type == VerticalNudgeType::OCTAVE) {
		if (isScaleModeClip()) {
			change *= modelStack->song->key.modeNotes.count();
		}
		else {
			change *= 12;
		}
	}

	// Make sure no notes sounding
	stopAllNotesPlaying(modelStack);

	if (!isScaleModeClip()) {
		// Non scale clip — transpose directly by semitone jumps
		for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
			NoteRow* thisNoteRow = noteRows.getElement(i);
			thisNoteRow->y += change;
		}
	}
	else {
		// Scale clip — transpose by scale note jumps
		if (std::abs(change) == modelStack->song->key.modeNotes.count()) {
			int32_t changeInSemitones = (change > 0) ? 12 : -12;
			for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
				NoteRow* thisNoteRow = noteRows.getElement(i);
				thisNoteRow->y += changeInSemitones;
			}
		}
		else {
			for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
				MusicalKey key = modelStack->song->key;
				NoteRow* thisNoteRow = noteRows.getElement(i);
				int32_t changeInSemitones = 0;
				int32_t yNoteWithinOctave = key.intervalOf(thisNoteRow->getNoteCode());
				int32_t oldModeNoteIndex = 0;
				for (; oldModeNoteIndex < key.modeNotes.count(); oldModeNoteIndex++) {
					if (key.modeNotes[oldModeNoteIndex] == yNoteWithinOctave) {
						break;
					}
				}
				int32_t newModeNoteIndex = (oldModeNoteIndex + change + modelStack->song->key.modeNotes.count())
				                           % modelStack->song->key.modeNotes.count();

				if ((change > 0 && newModeNoteIndex > oldModeNoteIndex)
				    || (change < 0 && newModeNoteIndex < oldModeNoteIndex)) {
					changeInSemitones = modelStack->song->key.modeNotes[newModeNoteIndex]
					                    - modelStack->song->key.modeNotes[oldModeNoteIndex];
				}
				else {
					if (change > 0) {
						changeInSemitones = modelStack->song->key.modeNotes[newModeNoteIndex]
						                    - modelStack->song->key.modeNotes[oldModeNoteIndex] + 12;
					}
					else {
						changeInSemitones = modelStack->song->key.modeNotes[newModeNoteIndex]
						                    - modelStack->song->key.modeNotes[oldModeNoteIndex] - 12;
					}
				}
				thisNoteRow->y += changeInSemitones;
			}
		}
	}
	yScroll += change;
}

void MelodicClip::replaceMusicalMode(const ScaleChange& changes, ModelStackWithTimelineCounter* modelStack) {
	if (!isScaleModeClip()) {
		return;
	}
	MusicalKey key = modelStack->song->key;
	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		int8_t degree = key.degreeOf(thisNoteRow->y);
		if (degree >= 0) {
			ModelStackWithNoteRow* modelStackWithNoteRow =
			    modelStack->addNoteRow(getNoteRowId(thisNoteRow, i), thisNoteRow);

			thisNoteRow->stopCurrentlyPlayingNote(modelStackWithNoteRow);
			thisNoteRow->y += changes[degree];
		}
	}

	uint8_t oldSize = changes.source.scaleSize();
	uint8_t newSize = changes.target.scaleSize();

	int yOctave = yScroll / oldSize;
	int yDegree = yScroll - (yOctave * oldSize);
	yScroll = yOctave * newSize + yDegree;
}

void MelodicClip::noteRemovedFromMode(int32_t yNoteWithinOctave, Song* /*song*/) {
	if (!isScaleModeClip()) {
		return;
	}

	for (int32_t i = 0; i < noteRows.getNumElements();) {
		NoteRow* thisNoteRow = noteRows.getElement(i);

		if ((thisNoteRow->y + 120) % 12 == yNoteWithinOctave) {
			noteRows.deleteNoteRowAtIndex(i);
		}
		else {
			i++;
		}
	}
}

void MelodicClip::seeWhatNotesWithinOctaveArePresent(NoteSet& notesWithinOctavePresent, MusicalKey key) {
	for (int32_t i = 0; i < noteRows.getNumElements();) {
		NoteRow* thisNoteRow = noteRows.getElement(i);

		if (!thisNoteRow->hasNoNotes()) {
			notesWithinOctavePresent.add(key.intervalOf(thisNoteRow->getNoteCode()));
			i++;
		}
		else {
			// If this NoteRow has no notes, delete it to avoid problems during mode changes
			noteRows.deleteNoteRowAtIndex(i);
		}
	}
}

ModelStackWithNoteRow* MelodicClip::getOrCreateNoteRowForYNote(int32_t yNote, ModelStackWithTimelineCounter* modelStack,
                                                               Action* action, bool* scaleAltered) {
	ModelStackWithNoteRow* modelStackWithNoteRow = getNoteRowForYNote(yNote, modelStack);

	// If one didn't already exist, create one
	if (!modelStackWithNoteRow->getNoteRowAllowNull()) {
		int32_t noteRowIndex;
		NoteRow* thisNoteRow = noteRows.insertNoteRowAtY(yNote, &noteRowIndex);

		if (thisNoteRow) {
			// Check that this yNote is allowed within our scale, if we have a scale
			if (!modelStack->song->isYNoteAllowed(yNote, inScaleMode)) {

				if (scaleAltered) {
					*scaleAltered = true;
				}

				// Recalculate the scale
				int32_t newI = thisNoteRow->notes.insertAtKey(
				    0); // Total hack — make it look like the NoteRow has a Note so it doesn't get discarded
				        // during setRootNote(). We set it back really soon.
				modelStack->song->setRootNote(modelStack->song->key.rootNote);

				thisNoteRow = getNoteRowForYNote(yNote); // Must re-get it
				if (ALPHA_OR_BETA_VERSION && !thisNoteRow) {
					FREEZE_WITH_ERROR("E -1");
				}

				thisNoteRow->notes.empty(); // Undo the total hack above

				if (action) {
					void* consMemory = allocExternal(sizeof(ConsequenceScaleAddNote));

					if (consMemory) {
						ConsequenceScaleAddNote* newConsequence =
						    new (consMemory) ConsequenceScaleAddNote((yNote + 120) % 12);
						action->addConsequence(newConsequence);
					}

					action->modeNotes[AFTER] = modelStack->song->key.modeNotes;
				}
			}

			modelStackWithNoteRow->setNoteRow(thisNoteRow, yNote);
		}
	}
	return modelStackWithNoteRow;
}

NoteRow* MelodicClip::createNewNoteRowForYVisual(int32_t yVisual, Song* song) {
	int32_t y = getYNoteFromYVisual(yVisual, song);
	return noteRows.insertNoteRowAtY(y);
}

int16_t MelodicClip::getTopYNote() {
	if (noteRows.getNumElements() == 0) {
		return 64;
	}
	return noteRows.getElement(noteRows.getNumElements() - 1)->y;
}

int16_t MelodicClip::getBottomYNote() {
	if (noteRows.getNumElements() == 0) {
		return 64;
	}
	return noteRows.getElement(0)->y;
}

bool MelodicClip::isScaleModeClip() {
	return inScaleMode;
}
