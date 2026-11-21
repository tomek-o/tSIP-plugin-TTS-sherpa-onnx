#ifndef TTS_UTILS_H
#define TTS_UTILS_H

#include <windows.h>
#include <string>

namespace Utils
{
	std::wstring Utf8ToWide(const std::string &s);
	std::string WideToUtf8(const std::wstring &s);
	std::wstring AnsiToWide(const std::string &s);
	/** Convert to system ANSI code page (tSIP and the sherpa-onnx file APIs use narrow ANSI strings) */
	std::string WideToAnsi(const std::wstring &s);
	/** Check if string can be represented in ANSI code page without loss */
	bool IsAnsiRepresentable(const std::wstring &s);

	HMODULE GetCurrentModule(void);
	/** Full path of the plugin DLL */
	std::wstring GetDllPath(void);
	std::wstring GetDllDir(void);
	std::wstring GetDllNameWithoutExtension(void);
	std::wstring ReplaceFileExtension(const std::wstring &path, const std::wstring &ext);

	bool IsAbsolutePath(const std::wstring &path);
	/** Return path unchanged if absolute, otherwise base + "\" + path; empty path stays empty */
	std::wstring ResolvePath(const std::wstring &base, const std::wstring &path);
	bool FileExists(const std::wstring &path);
	bool DirectoryExists(const std::wstring &path);

	std::wstring Trim(const std::wstring &s);
}

#endif
