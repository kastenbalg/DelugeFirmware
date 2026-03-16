#pragma once

#include "gui/menu_item/toggle.h"
#include "gui/ui/sound_editor.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::audio_compressor {

class SidechainListen : public Toggle {
public:
	using Toggle::Toggle;

	void readCurrentValue() override {
		if (soundEditor.currentSound != nullptr) {
			this->setValue(soundEditor.currentSound->sidechainListenEnabled);
		}
	}

	void writeCurrentValue() override {
		if (soundEditor.currentSound != nullptr) {
			soundEditor.currentSound->sidechainListenEnabled = this->getValue();
		}
	}
};

} // namespace deluge::gui::menu_item::audio_compressor
