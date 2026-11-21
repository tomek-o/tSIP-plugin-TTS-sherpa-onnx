#ifndef TTS_QUICK_TEXT_DIALOG_H
#define TTS_QUICK_TEXT_DIALOG_H

#include "Settings.h"
#include <windows.h>

namespace QuickTextDialog
{
	/** \brief Edit quick text button (caption, text, voice)
		\param voices voice list for voice selection
		\return true if OK was pressed
	*/
	bool Show(HINSTANCE hInstance, HWND parent, QuickText &qt, const std::vector<VoiceConfig> &voices);
}

#endif
