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

#include "definitions_cxx.hpp"

extern "C" {
#include "fatfs/ff.h"
}

class Song;
class FileItem;

/// Result of loading a song from file. On success, song is non-null and owns a fully
/// constructed Song with samples enqueued for loading. On failure, song is nullptr.
struct SongLoadResult {
	Song* song = nullptr;
	Error error = Error::NONE;
};

/// Load a song from a FileItem (app task path — FileItem must be valid).
SongLoadResult loadSongFromFile(FileItem* fileItem, const char* dirPath, bool loadAllSamples, const char* songName);

/// Load a song from a FilePointer + filename (storage task path — no FileItem needed).
SongLoadResult loadSongFromFilePointer(FilePointer* fp, const char* filename, const char* dirPath, bool loadAllSamples,
                                       const char* songName);
