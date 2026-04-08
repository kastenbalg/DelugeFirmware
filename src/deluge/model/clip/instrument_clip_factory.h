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

#include "definitions_cxx.hpp"
#include <cstddef>

class InstrumentClip;
class Song;

/// Allocation size for any InstrumentClip subclass.
/// All instrument clip creation sites must use this size so that the concrete
/// subclass (KitClip or MelodicClip) can be placed into the same allocation
/// without reallocation when the output type is known.
size_t instrumentClipAllocSize();

/// Create an InstrumentClip for a known output type (e.g. when creating a new
/// clip from the UI).  Returns nullptr on allocation failure.
InstrumentClip* createInstrumentClip(OutputType outputType, Song* song = nullptr);

/// Create an InstrumentClip for loading from a file.  The output type is not
/// yet known; a post-load upgrade pass will reconstruct the correct concrete
/// subclass once outputTypeWhileLoading has been set.
InstrumentClip* createInstrumentClipForLoading();
