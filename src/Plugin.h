/** \file
	\brief Plugin logic: window (separate or embedded in tSIP button), message queue, playback into call
	All functions are called from tSIP main (GUI) thread.
*/
#ifndef TTS_PLUGIN_H
#define TTS_PLUGIN_H

#include <windows.h>

#include "version.h"

namespace Plugin
{
	int Start(void);
	void Stop(void);
	void OnEvent(const char *type, const char *json);
	void ShowSettings(HWND parent);
}

#endif
