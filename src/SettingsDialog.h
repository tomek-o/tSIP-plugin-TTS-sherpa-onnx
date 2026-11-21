#ifndef TTS_SETTINGS_DIALOG_H
#define TTS_SETTINGS_DIALOG_H

#include "Settings.h"
#include <windows.h>
#include <string>

namespace SettingsDialog
{
	/** \brief Show modal settings dialog
		\param settings edited in place (only if OK is pressed)
		\param baseDir plugin data directory - base for relative paths
		\return true if OK was pressed
	*/
	bool Show(HINSTANCE hInstance, HWND parent, Settings &settings, const std::wstring &baseDir);
}

#endif
