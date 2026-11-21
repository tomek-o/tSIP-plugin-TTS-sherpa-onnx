#include "Utils.h"
#include <vector>

namespace
{

std::wstring MultiToWide(UINT cp, const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int size = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), NULL, 0);
	if (size <= 0)
		return std::wstring();
	std::vector<wchar_t> buf(size);
	MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &buf[0], size);
	return std::wstring(&buf[0], size);
}

std::string WideToMulti(UINT cp, const std::wstring &s)
{
	if (s.empty())
		return std::string();
	int size = WideCharToMultiByte(cp, 0, s.data(), (int)s.size(), NULL, 0, NULL, NULL);
	if (size <= 0)
		return std::string();
	std::vector<char> buf(size);
	WideCharToMultiByte(cp, 0, s.data(), (int)s.size(), &buf[0], size, NULL, NULL);
	return std::string(&buf[0], size);
}

}	// namespace

std::wstring Utils::Utf8ToWide(const std::string &s)
{
	return MultiToWide(CP_UTF8, s);
}

std::string Utils::WideToUtf8(const std::wstring &s)
{
	return WideToMulti(CP_UTF8, s);
}

std::wstring Utils::AnsiToWide(const std::string &s)
{
	return MultiToWide(CP_ACP, s);
}

std::string Utils::WideToAnsi(const std::wstring &s)
{
	return WideToMulti(CP_ACP, s);
}

bool Utils::IsAnsiRepresentable(const std::wstring &s)
{
	if (s.empty())
		return true;
	// "Use Unicode UTF-8 for worldwide language support": everything representable,
	// WC_NO_BEST_FIT_CHARS / lpUsedDefaultChar are not allowed for CP_UTF8
	if (GetACP() == CP_UTF8)
		return true;
	BOOL usedDefault = FALSE;
	int size = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.data(), (int)s.size(), NULL, 0, NULL, &usedDefault);
	return size > 0 && !usedDefault;
}

HMODULE Utils::GetCurrentModule(void)
{
	HMODULE hModule = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&Utils::GetCurrentModule), &hModule);
	return hModule;
}

std::wstring Utils::GetDllPath(void)
{
	std::vector<wchar_t> buf(MAX_PATH);
	for (;;)
	{
		DWORD len = GetModuleFileNameW(GetCurrentModule(), &buf[0], (DWORD)buf.size());
		if (len == 0)
			return std::wstring();
		if (len < buf.size())
			return std::wstring(&buf[0], len);
		buf.resize(buf.size() * 2);
	}
}

std::wstring Utils::GetDllDir(void)
{
	std::wstring path = GetDllPath();
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos)
		return std::wstring();
	return path.substr(0, pos);
}

std::wstring Utils::GetDllNameWithoutExtension(void)
{
	std::wstring path = GetDllPath();
	size_t pos = path.find_last_of(L"\\/");
	if (pos != std::wstring::npos)
		path = path.substr(pos + 1);
	pos = path.find_last_of(L'.');
	if (pos != std::wstring::npos)
		path = path.substr(0, pos);
	return path;
}

std::wstring Utils::ReplaceFileExtension(const std::wstring &path, const std::wstring &ext)
{
	size_t dot = path.find_last_of(L'.');
	size_t slash = path.find_last_of(L"\\/");
	if (dot == std::wstring::npos || (slash != std::wstring::npos && slash > dot))
		return path + ext;
	return path.substr(0, dot) + ext;
}

bool Utils::IsAbsolutePath(const std::wstring &path)
{
	if (path.size() >= 2 && path[1] == L':')
		return true;
	if (!path.empty() && (path[0] == L'\\' || path[0] == L'/'))
		return true;
	return false;
}

std::wstring Utils::ResolvePath(const std::wstring &base, const std::wstring &path)
{
	if (path.empty() || IsAbsolutePath(path) || base.empty())
		return path;
	std::wstring ret = base;
	if (ret[ret.size() - 1] != L'\\' && ret[ret.size() - 1] != L'/')
		ret += L'\\';
	return ret + path;
}

bool Utils::FileExists(const std::wstring &path)
{
	DWORD attr = GetFileAttributesW(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool Utils::DirectoryExists(const std::wstring &path)
{
	DWORD attr = GetFileAttributesW(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Utils::Trim(const std::wstring &s)
{
	const wchar_t *ws = L" \t\r\n";
	size_t start = s.find_first_not_of(ws);
	if (start == std::wstring::npos)
		return std::wstring();
	size_t end = s.find_last_not_of(ws);
	return s.substr(start, end - start + 1);
}
