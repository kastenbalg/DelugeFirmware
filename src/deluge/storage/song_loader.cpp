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

#include "storage/song_loader.h"
#include "extern.h"
#include "memory/memory_allocator_interface.h"
#include "model/global_effectable/global_effectable.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/file_item.h"
#include "storage/storage_manager.h"
#include "storage/storage_task.h"

extern "C" void terminate_set_context(const char* ctx);

/// Shared core: builds Song from already-opened deserializer.
/// @param dirPath        Directory path for the song
/// @param filename       Filename with extension (for getFilenameWithoutExtension)
/// @param loadAllSamples If true loads all, else only crucial samples
/// @param songName       Song display name
static SongLoadResult loadSongCore(const char* dirPath, const char* filename, bool loadAllSamples,
                                   const char* songName) {
	SongLoadResult result;

	terminate_set_context("TLD6");
	void* songMemory = allocExternal(sizeof(Song));
	terminate_set_context("TD6A");
	if (!songMemory) {
		result.error = Error::INSUFFICIENT_RAM;
		activeDeserializer->closeWriter();
		return result;
	}

	terminate_set_context("TD6B");
	Song* song = new (songMemory) Song();
	preLoadedSong = song;

	terminate_set_context("TD6C");
	result.error = song->paramManager.setupUnpatched();
	if (result.error != Error::NONE) {
		goto cleanupSong;
	}

	terminate_set_context("TD6D");
	GlobalEffectable::initParams(&song->paramManager);

	AudioEngine::logAction("initialized new song");

	terminate_set_context("TD6E");
	result.error = song->readFromFile(*activeDeserializer);
	if (result.error != Error::NONE) {
		goto cleanupSong;
	}
	AudioEngine::logAction("read new song from file");

	{
		FRESULT fresult = activeDeserializer->closeWriter();
		if (fresult != FR_OK) {
			result.error = Error::SD_CARD;
			goto cleanupSong;
		}
	}

	// Set song metadata
	song->dirPath.set(dirPath);

	{
		// Derive filename without extension for alternate audio dir
		const char* dot = strrchr(filename, '.');
		int32_t nameLen = dot ? (int32_t)(dot - filename) : (int32_t)strlen(filename);
		String filenameWithoutExt;
		filenameWithoutExt.set(filename, nameLen);

		result.error = audioFileManager.setupAlternateAudioFileDir(audioFileManager.alternateAudioFileLoadPath, dirPath,
		                                                           filenameWithoutExt);
		if (result.error != Error::NONE) {
			goto cleanupSong;
		}
	}

	audioFileManager.thingBeginningLoading(ThingType::SONG);

	song->loadAllSamples(false);

	if (loadAllSamples) {
		song->loadAllSamples(true);
	}
	else {
		song->loadCrucialSamplesOnly();
	}

	song->name.set(songName);

	result.song = song;
	return result;

cleanupSong:
	preLoadedSong = nullptr;
	void* toDealloc = dynamic_cast<void*>(song);
	song->~Song();
	delugeDealloc(toDealloc);
	activeDeserializer->closeWriter();
	return result;
}

SongLoadResult loadSongFromFile(FileItem* fileItem, const char* dirPath, bool loadAllSamples, const char* songName) {
	SongLoadResult result;

	terminate_set_context("TLD2");
	result.error = StorageManager::openDelugeFile(fileItem, "song");
	if (result.error != Error::NONE) {
		return result;
	}

	// Get filename for the core loader
	String filenameWithExt;
	fileItem->getFilenameWithExtension(&filenameWithExt);

	return loadSongCore(dirPath, filenameWithExt.get(), loadAllSamples, songName);
}

SongLoadResult loadSongFromFilePointer(FilePointer* fp, const char* filename, const char* dirPath, bool loadAllSamples,
                                       const char* songName) {
	SongLoadResult result;

	terminate_set_context("TLD2");

	// Determine XML vs JSON from filename extension
	bool isJson = false;
	const char* dot = strrchr(filename, '.');
	if (dot && (strcasecmp(dot, ".Json") == 0 || strcasecmp(dot, ".json") == 0)) {
		isJson = true;
	}

	if (isJson) {
		result.error = StorageManager::openJsonFile(fp, smJsonDeserializer, "song", "", false);
	}
	else {
		result.error = StorageManager::openXMLFile(fp, smDeserializer, "song", "", false);
	}

	if (result.error != Error::NONE) {
		return result;
	}

	return loadSongCore(dirPath, filename, loadAllSamples, songName);
}
