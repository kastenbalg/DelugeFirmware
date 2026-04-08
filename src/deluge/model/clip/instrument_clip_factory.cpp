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

#include "model/clip/instrument_clip_factory.h"
#include "memory/memory_allocator_interface.h"
#include "model/clip/instrument_clip.h"
#include "model/clip/kit_clip.h"
#include "model/clip/melodic_clip.h"
#include <algorithm>
#include <new>

size_t instrumentClipAllocSize() {
	return std::max(sizeof(KitClip), sizeof(MelodicClip));
}

InstrumentClip* createInstrumentClip(OutputType outputType, Song* song) {
	void* memory = allocExternal(instrumentClipAllocSize());
	if (!memory) {
		return nullptr;
	}
	if (outputType == OutputType::KIT) {
		return new (memory) KitClip(song);
	}
	return new (memory) MelodicClip(song);
}

InstrumentClip* createInstrumentClipForLoading() {
	void* memory = allocExternal(instrumentClipAllocSize());
	if (!memory) {
		return nullptr;
	}
	// Base InstrumentClip used during loading; upgraded post-load by upgradeInstrumentClipsPostLoad()
	return new (memory) InstrumentClip();
}
