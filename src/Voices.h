/** \file
	\brief Voice directories (extracted sherpa-onnx TTS model archives) in plugin data directory
*/
#ifndef TTS_VOICES_H
#define TTS_VOICES_H

#include "Settings.h"
#include <string>
#include <vector>

namespace Voices
{
	/** Path relative to base directory if path is inside it, otherwise unchanged */
	std::wstring MakeRelative(const std::wstring &baseDir, const std::wstring &path);

	/** First file (not directory) matching pattern in directory */
	bool FindFile(const std::wstring &dir, const wchar_t *pattern, std::wstring &name);

	/** Voice configuration from model directory as distributed by sherpa-onnx
		(e.g. vits-piper-pl_PL-gosia-medium); paths relative to baseDir if possible
		\return false if there is no model in directory
	*/
	bool MakeVoiceFromDir(const std::wstring &baseDir, const std::wstring &dir, VoiceConfig &v);

	/** Voices installed in subdirectories of baseDir, sorted by directory name */
	std::vector<VoiceConfig> FindInstalled(const std::wstring &baseDir);

	/** Check if model file of the voice exists */
	bool IsInstalled(const std::wstring &baseDir, const VoiceConfig &v);
}

#endif
