#ifndef TTS_DOWNLOAD_DIALOG_H
#define TTS_DOWNLOAD_DIALOG_H

#include <windows.h>
#include <string>
#include <vector>

namespace DownloadDialog
{
	/** \brief Modal dialog: download / install voices into plugin data directory
		\param installedDirs receives full paths of installed voice directories
	*/
	void Show(HINSTANCE hInstance, HWND parent, const std::wstring &baseDir, std::vector<std::wstring> &installedDirs);
}

#endif
