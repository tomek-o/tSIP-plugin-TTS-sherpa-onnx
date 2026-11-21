#include "Voices.h"
#include "Utils.h"
#include <windows.h>
#include <algorithm>

/** Path relative to base directory if file is inside it */
std::wstring Voices::MakeRelative(const std::wstring &baseDir, const std::wstring &path)
{
	std::wstring prefix = baseDir + L"\\";
	if (path.size() > prefix.size() && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, path.c_str(), static_cast<int>(prefix.size()),
		prefix.c_str(), static_cast<int>(prefix.size())) == CSTR_EQUAL)
	{
		return path.substr(prefix.size());
	}
	return path;
}

bool Voices::FindFile(const std::wstring &dir, const wchar_t *pattern, std::wstring &name)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	bool found = false;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			name = fd.cFileName;
			found = true;
			break;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return found;
}

/** Voice configuration from model directory as distributed by sherpa-onnx (e.g. vits-piper-pl_PL-gosia-medium)
	\return false if there is no model in directory
*/
namespace
{

/** .onnx model in directory; if there are more: prefer full model over int8 / fp16 variants */
bool FindModel(const std::wstring &dir, std::wstring &name)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*.onnx").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	std::wstring fallback;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		std::wstring n = fd.cFileName;
		std::wstring lower = n;
		CharLowerBuffW(&lower[0], static_cast<DWORD>(lower.size()));
		if (lower.find(L"int8") == std::wstring::npos && lower.find(L"fp16") == std::wstring::npos)
		{
			name = n;
			FindClose(h);
			return true;
		}
		if (fallback.empty())
			fallback = n;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	name = fallback;
	return !name.empty();
}

}	// namespace

bool Voices::MakeVoiceFromDir(const std::wstring &baseDir, const std::wstring &dir, VoiceConfig &v)
{
	std::wstring model, tokens, voices, lexicon;
	if (!FindModel(dir, model))
		return false;
	std::wstring rel = Voices::MakeRelative(baseDir, dir);
	std::wstring dirName = dir.substr(dir.find_last_of(L"\\/") + 1);
	std::wstring lower = dirName;
	CharLowerBuffW(&lower[0], static_cast<DWORD>(lower.size()));

	v = VoiceConfig();
	v.modelType = "vits";
	if (lower.find(L"kokoro") != std::wstring::npos)
		v.modelType = "kokoro";
	else if (lower.find(L"kitten") != std::wstring::npos)
		v.modelType = "kitten";
	// e.g. vits-piper-pl_PL-gosia-medium-int8 -> pl_PL-gosia-medium-int8
	std::wstring name = dirName;
	if (name.compare(0, 11, L"vits-piper-") == 0)
		name.erase(0, 11);
	v.name = Utils::WideToUtf8(name);
	v.model = Utils::WideToUtf8(rel + L"\\" + model);
	if (Voices::FindFile(dir, L"tokens.txt", tokens))
		v.tokens = Utils::WideToUtf8(rel + L"\\" + tokens);
	if (Utils::DirectoryExists(dir + L"\\espeak-ng-data"))
		v.dataDir = Utils::WideToUtf8(rel + L"\\espeak-ng-data");
	if (Voices::FindFile(dir, L"voices.bin", voices))
		v.voices = Utils::WideToUtf8(rel + L"\\" + voices);
	if (v.modelType == "kokoro" && Voices::FindFile(dir, L"lexicon-us-en.txt", lexicon))
		v.lexicon = Utils::WideToUtf8(rel + L"\\" + lexicon);
	return true;
}

std::vector<VoiceConfig> Voices::FindInstalled(const std::wstring &baseDir)
{
	std::vector<std::wstring> dirs;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((baseDir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			std::wstring name = fd.cFileName;
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..")
				dirs.push_back(name);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(dirs.begin(), dirs.end());
	std::vector<VoiceConfig> voices;
	for (unsigned int i = 0; i < dirs.size(); i++)
	{
		VoiceConfig v;
		if (MakeVoiceFromDir(baseDir, baseDir + L"\\" + dirs[i], v))
			voices.push_back(v);
	}
	return voices;
}

bool Voices::IsInstalled(const std::wstring &baseDir, const VoiceConfig &v)
{
	return !v.model.empty() && Utils::FileExists(Utils::ResolvePath(baseDir, Utils::Utf8ToWide(v.model)));
}
