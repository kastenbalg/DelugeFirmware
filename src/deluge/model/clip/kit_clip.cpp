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

#include "model/clip/kit_clip.h"
#include "gui/views/arranger_view.h"
#include "gui/views/session_view.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/scale/musical_key.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"

// -----------------------------------------------------------------------
// Y-coordinate virtual overrides
// -----------------------------------------------------------------------

NoteRow* KitClip::getNoteRowOnScreen(int32_t yDisplay, Song* /*song*/, int32_t* getIndex) {
	int32_t i = yDisplay + yScroll;
	if (i < 0 || i >= noteRows.getNumElements()) {
		return nullptr;
	}
	if (getIndex) {
		*getIndex = i;
	}
	return noteRows.getElement(i);
}

ModelStackWithNoteRow* KitClip::getNoteRowOnScreen(int32_t yDisplay, ModelStackWithTimelineCounter* modelStack) {
	int32_t noteRowIndex;
	NoteRow* noteRow = getNoteRowOnScreen(yDisplay, modelStack->song, &noteRowIndex);
	int32_t noteRowId = 0;
	if (noteRow) {
		noteRowId = getNoteRowId(noteRow, noteRowIndex);
	}
	return modelStack->addNoteRow(noteRowId, noteRow);
}

// -----------------------------------------------------------------------
// Note tail virtual override
// -----------------------------------------------------------------------

bool KitClip::allowNoteTails(ModelStackWithNoteRow* modelStack) {
	NoteRow* noteRow = modelStack->getNoteRowAllowNull();
	if (!noteRow || !noteRow->drum) {
		return true;
	}
	ModelStackWithSoundFlags* modelStackWithSoundFlags =
	    modelStack->addOtherTwoThings(noteRow->drum->toModControllable(), &noteRow->paramManager)->addSoundFlags();
	return noteRow->drum->allowNoteTails(modelStackWithSoundFlags);
}

// -----------------------------------------------------------------------
// ModControllable/ParamManager derivation
// -----------------------------------------------------------------------

void KitClip::getActiveModControllable(ModelStackWithTimelineCounter* modelStack) {
	if (!affectEntire && getRootUI() != &sessionView && getRootUI() != &arrangerView) {
		Kit* kit = (Kit*)output;

		if (!kit->selectedDrum || kit->selectedDrum->type != DrumType::SOUND) {
returnNull:
			modelStack->setTimelineCounter(nullptr);
			modelStack->addOtherTwoThingsButNoNoteRow(nullptr, nullptr);
		}
		else {
			int32_t noteRowIndex;
			NoteRow* noteRow = getNoteRowForDrum(kit->selectedDrum, &noteRowIndex);

			if (!noteRow) {
				goto returnNull;
			}

			modelStack->addNoteRow(noteRowIndex, noteRow)
			    ->addOtherTwoThings((SoundDrum*)kit->selectedDrum, &noteRow->paramManager);
		}
	}
	else {
		Clip::getActiveModControllable(modelStack);
	}
}

ModControllable* KitClip::getModControllableForNoteRow(NoteRow* noteRow) {
	if (noteRow && noteRow->drum) {
		return noteRow->drum->toModControllable();
	}
	return output ? output->toModControllable() : nullptr;
}

ParamManager* KitClip::getParamManagerForNoteRow(NoteRow* noteRow) {
	if (noteRow && noteRow->drum) {
		return &noteRow->paramManager;
	}
	return &paramManager;
}

// -----------------------------------------------------------------------
// Kit-specific methods (moved from InstrumentClip in Phase 1.3)
// -----------------------------------------------------------------------

NoteRow* KitClip::createNewNoteRowForKit(ModelStackWithTimelineCounter* modelStack, bool atStart, int32_t* getIndex) {
	int32_t index = atStart ? 0 : noteRows.getNumElements();

	Drum* newDrum = ((Kit*)output)->getFirstUnassignedDrum(this);

	NoteRow* newNoteRow = noteRows.insertNoteRowAtIndex(index);
	if (!newNoteRow) {
		return nullptr;
	}

	ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(index, newNoteRow);

	newNoteRow->setDrum(newDrum, (Kit*)output, modelStackWithNoteRow); // It might end up NULL. That's fine

	if (atStart) {
		yScroll++;
		// Adjust colour offset — relative to the lowest NoteRow, which we just changed.
		colourOffset--;
	}

	if (getIndex) {
		*getIndex = index;
	}
	return newNoteRow;
}

void KitClip::assignDrumsToNoteRows(ModelStackWithTimelineCounter* modelStack, bool shouldGiveMIDICommandsToDrums,
                                    int32_t numNoteRowsPreviouslyDeletedFromBottom) {
	Kit* kit = (Kit*)output;

	Drum* nextPotentiallyUnassignedDrum = kit->firstDrum;

	// We first need to know whether any NoteRows already have a Drum
	int32_t firstNoteRowToHaveADrum = -1;
	Drum* lowestDrumOnscreen = nullptr;
	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		if (thisNoteRow->drum) {
			firstNoteRowToHaveADrum = i;
			lowestDrumOnscreen = thisNoteRow->drum;
			break;
		}
	}

	int32_t maxNumNoteRowsToInsertAtBottom;

	// If at least one NoteRow already did have a Drum, put the first unassigned drums (up to the first assigned
	// one) and their new NoteRows at the bottom of the screen
	if (firstNoteRowToHaveADrum >= 0) {

		// If first NoteRow already had a Drum, we can insert as many new ones below it as we want
		if (firstNoteRowToHaveADrum == 0) {
			maxNumNoteRowsToInsertAtBottom = 2147483647;

			// Otherwise, only allow enough new ones to be inserted that, combined with the drum-less ones at the
			// bottom, it'll take us up to the drum in question
		}
		else {
			maxNumNoteRowsToInsertAtBottom = kit->getDrumIndex(lowestDrumOnscreen) - firstNoteRowToHaveADrum;
		}

insertSomeAtBottom:
		int32_t numNoteRowsInsertedAtBottom = 0;

		while (nextPotentiallyUnassignedDrum && numNoteRowsInsertedAtBottom < maxNumNoteRowsToInsertAtBottom) {

			Drum* thisDrum = nextPotentiallyUnassignedDrum;
			nextPotentiallyUnassignedDrum = nextPotentiallyUnassignedDrum->next;

			// If this Drum is already assigned to a NoteRow...
			if (thisDrum->noteRowAssignedTemp) {
				break;
			}

			// Create the NoteRow
			NoteRow* newNoteRow = noteRows.insertNoteRowAtIndex(numNoteRowsInsertedAtBottom);
			if (!newNoteRow) {
				break;
			}

			int32_t noteRowId = getNoteRowId(newNoteRow, numNoteRowsInsertedAtBottom);
			ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(noteRowId, newNoteRow);

			newNoteRow->setDrum(thisDrum, kit, modelStackWithNoteRow);
			numNoteRowsInsertedAtBottom++;
		}
		yScroll += numNoteRowsInsertedAtBottom;
	}

	else {
		if (numNoteRowsPreviouslyDeletedFromBottom > 0) {
			// We don't actually get here very often at all
			maxNumNoteRowsToInsertAtBottom = numNoteRowsPreviouslyDeletedFromBottom;
			goto insertSomeAtBottom;
		}
	}

	bool anyNoteRowsRemainingWithoutDrum = false;

	// For any NoteRow without a Drum assigned, give it an unused Drum if there is one
	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		if (!thisNoteRow->drum) {

			if (!nextPotentiallyUnassignedDrum) {
noUnassignedDrumsLeft:
				anyNoteRowsRemainingWithoutDrum = true;
				continue;
			}

			while (nextPotentiallyUnassignedDrum->noteRowAssignedTemp) {
				nextPotentiallyUnassignedDrum = nextPotentiallyUnassignedDrum->next;
				if (!nextPotentiallyUnassignedDrum) {
					goto noUnassignedDrumsLeft;
				}
			}

			int32_t noteRowId = getNoteRowId(thisNoteRow, i);
			ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(noteRowId, thisNoteRow);

			thisNoteRow->setDrum(nextPotentiallyUnassignedDrum, kit, modelStackWithNoteRow);
			nextPotentiallyUnassignedDrum = nextPotentiallyUnassignedDrum->next;

			if (shouldGiveMIDICommandsToDrums) {
				thisNoteRow->giveMidiCommandsToDrum();
			}
		}
	}

	// If any NoteRows with no Drum remain (more NoteRows than Drums), delete them if at the end and empty
	if (anyNoteRowsRemainingWithoutDrum) {
		deleteEmptyNoteRowsAtEitherEnd(true, modelStack);
	}

	// Or, if all NoteRows have a Drum, check if there are any Drums without a NoteRow and make them one
	else {
		for (; nextPotentiallyUnassignedDrum; nextPotentiallyUnassignedDrum = nextPotentiallyUnassignedDrum->next) {

			// If this Drum is already assigned to a NoteRow...
			if (nextPotentiallyUnassignedDrum->noteRowAssignedTemp) {
				continue;
			}

			// Create the NoteRow
			int32_t i = noteRows.getNumElements();
			NoteRow* newNoteRow = noteRows.insertNoteRowAtIndex(i);
			if (!newNoteRow) {
				break;
			}

			int32_t noteRowId = getNoteRowId(newNoteRow, i);
			ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(noteRowId, newNoteRow);

			newNoteRow->setDrum(nextPotentiallyUnassignedDrum, kit, modelStackWithNoteRow);
		}
	}
}

void KitClip::unassignAllNoteRowsFromDrums(ModelStackWithTimelineCounter* modelStack, bool shouldRememberDrumNames,
                                           bool shouldRetainLinksToSounds, bool shouldGrabMidiCommands,
                                           bool shouldBackUpExpressionParamsToo) {
	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		if (thisNoteRow->drum) {
			if (shouldRememberDrumNames) {
				thisNoteRow->rememberDrumName();
			}
			AudioEngine::logAction("KitClip::unassignAllNoteRowsFromDrums");
			AudioEngine::routineWithClusterLoading();

			if (shouldRetainLinksToSounds) {
				if (thisNoteRow->paramManager.containsAnyMainParamCollections()) {
					modelStack->song->backUpParamManager((SoundDrum*)thisNoteRow->drum, this,
					                                     &thisNoteRow->paramManager, shouldBackUpExpressionParamsToo);
				}
			}
			else {
				if (shouldGrabMidiCommands) {
					thisNoteRow->grabMidiCommandsFromDrum();
				}

				int32_t noteRowId = getNoteRowId(thisNoteRow, i);
				ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(noteRowId, thisNoteRow);
				thisNoteRow->setDrum(nullptr, (Kit*)output, modelStackWithNoteRow);
			}
		}
	}
}

ModelStackWithNoteRow* KitClip::getNoteRowForSelectedDrum(ModelStackWithTimelineCounter* modelStack) {
	int32_t noteRowId;
	NoteRow* noteRow = nullptr;
	Kit* kit = (Kit*)output;
	if (kit->selectedDrum) {
		noteRow = getNoteRowForDrum(kit->selectedDrum, &noteRowId);
	}
	return modelStack->addNoteRow(noteRowId, noteRow);
}

ModelStackWithNoteRow* KitClip::getNoteRowForDrum(ModelStackWithTimelineCounter* modelStack, Drum* drum) {
	int32_t noteRowId;
	NoteRow* noteRow = getNoteRowForDrum(drum, &noteRowId);
	return modelStack->addNoteRow(noteRowId, noteRow);
}

NoteRow* KitClip::getNoteRowForDrum(Drum* drum, int32_t* getIndex) {
	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		if (thisNoteRow->drum == drum) {
			if (getIndex) {
				*getIndex = i;
			}
			return thisNoteRow;
		}
	}
	return nullptr;
}

ModelStackWithNoteRow* KitClip::getNoteRowForDrumName(ModelStackWithTimelineCounter* modelStack, char const* name) {
	int32_t i;
	NoteRow* thisNoteRow;

	for (i = 0; i < noteRows.getNumElements(); i++) {
		thisNoteRow = noteRows.getElement(i);
		if (thisNoteRow->drum && thisNoteRow->paramManager.containsAnyMainParamCollections()
		    && thisNoteRow->drum->type == DrumType::SOUND) {
			SoundDrum* thisDrum = (SoundDrum*)thisNoteRow->drum;

			if (thisDrum->name.equalsCaseIrrespective(name)) {
				goto foundIt;
			}
		}
	}

	thisNoteRow = nullptr;

foundIt:
	return modelStack->addNoteRow(i, thisNoteRow);
}

void KitClip::prepNoteRowsForExitingKitMode(Song* song) {

	// If for some reason no NoteRows, just return. This shouldn't ever happen
	if (noteRows.getNumElements() == 0) {
		return;
	}

	// We want to select one NoteRow, pinned to a yNote

	NoteRow* chosenNoteRow = nullptr;
	int32_t chosenNoteRowIndex;
	MusicalKey key = song->key;

	// If we're in scale mode...
	if (inScaleMode) {

		// See if any NoteRows are a root note
		for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
			NoteRow* thisNoteRow = noteRows.getElement(i);
			if (thisNoteRow->y != -32768 && key.intervalOf(thisNoteRow->y) == 0) {
				chosenNoteRow = thisNoteRow;
				chosenNoteRowIndex = i;
				break;
			}
		}
	}

	// If none found yet, just grab the first one with a "valid" yNote
	if (!chosenNoteRow) {

		for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
			NoteRow* thisNoteRow = noteRows.getElement(i);
			if (thisNoteRow->y != -32768) {

				// But, if we're in key-mode, make sure this yNote fits within the scale!
				if (inScaleMode) {
					uint8_t yNoteWithinOctave = key.intervalOf(thisNoteRow->y);

					// Make sure this yNote fits the scale/mode
					if (!song->key.modeNotes.has(yNoteWithinOctave)) {
						goto noteRowFailed;
					}
				}

				chosenNoteRow = thisNoteRow;
				chosenNoteRowIndex = i;
				break;
			}
noteRowFailed: {}
		}
	}

	// Occasionally we get a crazy scroll value. Not sure how. It happened to Jon Hutton
	if (chosenNoteRow) {
		if (chosenNoteRow->y < -256 || chosenNoteRow->y >= 256) {
			goto useRootNote;
		}
	}

	// If still none, just pick the first one
	else {
		chosenNoteRow = noteRows.getElement(0);
		chosenNoteRowIndex = 0;
useRootNote:
		chosenNoteRow->y = (song->key.rootNote % 12) + 60;
	}

	// Now give all the other NoteRows yNotes
	int32_t chosenNoteRowYVisual = song->getYVisualFromYNote(chosenNoteRow->y, inScaleMode);

	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		if (i != chosenNoteRowIndex) {
			thisNoteRow->y = song->getYNoteFromYVisual(chosenNoteRowYVisual - chosenNoteRowIndex + i, inScaleMode);
		}
	}
}

void KitClip::setupAsNewKitClipIfNecessary(ModelStackWithTimelineCounter* modelStack) {
	((Kit*)output)->resetDrumTempValues();
	assignDrumsToNoteRows(modelStack);
	yScroll = 0;
}

void KitClip::ensureScrollWithinKitBounds() {
	if (yScroll < 1 - kDisplayHeight) {
		yScroll = 1 - kDisplayHeight;
	}
	else {
		int32_t maxYScroll = getNumNoteRows() - 1;
		if (yScroll > maxYScroll) {
			yScroll = maxYScroll;
		}
	}
}

void KitClip::deleteOldDrumNames() {
	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);
		thisNoteRow->deleteOldDrumNames();
	}
}

// -----------------------------------------------------------------------
// Private
// -----------------------------------------------------------------------

void KitClip::prepareToEnterKitMode(Song* song) {
	// Make sure all rows on screen have a NoteRow. Any RAM problems and we'll just quit
	for (int32_t yDisplay = 0; yDisplay < kDisplayHeight; yDisplay++) {
		NoteRow* noteRow = getNoteRowOnScreen(yDisplay, song);
		if (!noteRow) {
			noteRow = createNewNoteRowForYVisual(yDisplay + yScroll, song);
			if (!noteRow) {
				return;
			}
		}
	}

	// Delete empty NoteRows that aren't onscreen
	for (int32_t i = 0; i < noteRows.getNumElements(); i++) {
		NoteRow* thisNoteRow = noteRows.getElement(i);

		int32_t yDisplay = getYVisualFromYNote(thisNoteRow->y, song) - yScroll;

		if ((yDisplay < 0 || yDisplay >= kDisplayHeight) && thisNoteRow->hasNoNotes()) {
			noteRows.deleteNoteRowAtIndex(i);
		}
		else {
			i++;
		}
	}

	// Figure out the new scroll value
	if (noteRows.getNumElements()) {
		yScroll -= getYVisualFromYNote(noteRows.getElement(0)->y, song);
	}
	else {
		yScroll = 0;
	}
}
