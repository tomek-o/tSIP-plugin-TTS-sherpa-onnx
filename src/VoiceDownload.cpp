#include "VoiceDownload.h"
#include "Utils.h"
#include "Log.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <json/json.h>
#include "bzlib.h"

using namespace VoiceDownload;

namespace
{

const char* const CATALOG_URL = "https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/tags/tts-models";
const wchar_t* const USER_AGENT = L"tSIP-TTS-sherpa-onnx-plugin/1.0";

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

std::string ErrorText(const char *what, DWORD err)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "%s failed (error %lu)", what, err);
	return buf;
}

class HttpHandle
{
public:
	explicit HttpHandle(HINTERNET h = NULL): handle(h) {}
	~HttpHandle(void)
	{
		if (handle)
			WinHttpCloseHandle(handle);
	}
	HINTERNET handle;
private:
	HttpHandle(const HttpHandle&);
	HttpHandle& operator=(const HttpHandle&);
};

/** HTTP GET; data passed to sink: return false from sink to abort */
typedef bool (*SinkFn)(void *arg, const char *data, DWORD len);

int HttpGet(const std::string &url, SinkFn sink, void *sinkArg, unsigned long long &contentLength,
	volatile long *cancel, std::string &error)
{
	contentLength = 0;
	std::wstring wurl = Utils::Utf8ToWide(url);
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256], path[2048];
	uc.lpszHostName = host;
	uc.dwHostNameLength = sizeof(host)/sizeof(host[0]);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = sizeof(path)/sizeof(path[0]);
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
	{
		error = "invalid URL: " + url;
		return 1;
	}

	// WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (Windows 8.1+) uses system proxy settings; fall back for older systems
	HttpHandle session(WinHttpOpen(USER_AGENT, 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (session.handle == NULL)
		session.handle = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session.handle == NULL)
	{
		error = ErrorText("WinHttpOpen", GetLastError());
		return 1;
	}
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
	{
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	}
	WinHttpSetTimeouts(session.handle, 30000, 30000, 30000, 60000);

	HttpHandle connection(WinHttpConnect(session.handle, host, uc.nPort, 0));
	if (connection.handle == NULL)
	{
		error = ErrorText("WinHttpConnect", GetLastError());
		return 1;
	}
	HttpHandle request(WinHttpOpenRequest(connection.handle, L"GET", path, NULL, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
	if (request.handle == NULL)
	{
		error = ErrorText("WinHttpOpenRequest", GetLastError());
		return 1;
	}
	if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request.handle, NULL))
	{
		DWORD err = GetLastError();
		error = ErrorText("HTTP request", err);
		if (err == ERROR_WINHTTP_SECURE_FAILURE)
			error += " - TLS error (TLS 1.2 not available?)";
		else if (err == ERROR_WINHTTP_NAME_NOT_RESOLVED || err == ERROR_WINHTTP_CANNOT_CONNECT)
			error += " - no connection to server";
		return 1;
	}
	DWORD status = 0, size = sizeof(status);
	WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
		&status, &size, WINHTTP_NO_HEADER_INDEX);
	if (status != 200)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "HTTP status %lu", status);
		error = buf;
		if (status == 403)
			error += " (GitHub API rate limit? try again later)";
		return 1;
	}
	wchar_t lenBuf[32];
	size = sizeof(lenBuf);
	if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &size, WINHTTP_NO_HEADER_INDEX))
		contentLength = _wcstoui64(lenBuf, NULL, 10);

	std::vector<char> buf(64 * 1024);
	for (;;)
	{
		if (cancel && *cancel)
		{
			error = "cancelled";
			return 2;
		}
		DWORD read = 0;
		if (!WinHttpReadData(request.handle, &buf[0], static_cast<DWORD>(buf.size()), &read))
		{
			error = ErrorText("receiving data", GetLastError());
			return 1;
		}
		if (read == 0)
			break;
		if (!sink(sinkArg, &buf[0], read))
		{
			if (error.empty())
				error = "failed to store received data";
			return 1;
		}
	}
	return 0;
}

bool StringSink(void *arg, const char *data, DWORD len)
{
	std::string *s = static_cast<std::string*>(arg);
	if (s->size() > 32 * 1024 * 1024)
		return false;
	s->append(data, len);
	return true;
}

struct FileSink
{
	FILE *fp;
	unsigned long long done;
	const unsigned long long *total;	///< content length, known after response headers (0 = unknown)
	ProgressFn progress;
	void *arg;
};

bool FileSinkFn(void *arg, const char *data, DWORD len)
{
	FileSink *fs = static_cast<FileSink*>(arg);
	if (fwrite(data, 1, len, fs->fp) != len)
		return false;
	fs->done += len;
	if (fs->progress)
		fs->progress(fs->arg, fs->done, *fs->total);
	return true;
}

bool IsSupportedArchive(const std::string &name)
{
	const std::string ext = ".tar.bz2";
	if (name.size() <= ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
		return false;
	// fp16 models: not supported by 32-bit sherpa-onnx 1.13.8 / onnxruntime (exception when loading)
	if (name.find("-fp16") != std::string::npos)
		return false;
	return name.compare(0, 5, "vits-") == 0 || name.compare(0, 7, "kokoro-") == 0 || name.compare(0, 7, "kitten-") == 0;
}

bool AssetLess(const Asset &a, const Asset &b)
{
	return a.name < b.name;
}

bool CreateDirectories(const std::wstring &dir)
{
	if (dir.empty() || Utils::DirectoryExists(dir))
		return true;
	size_t pos = dir.find_last_of(L'\\');
	if (pos != std::wstring::npos && pos > 2)
	{
		if (!CreateDirectories(dir.substr(0, pos)))
			return false;
	}
	return CreateDirectoryW(dir.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/** Reading exact amounts from bzip2 stream */
class Bz2Reader
{
public:
	Bz2Reader(void): fp(NULL), bz(NULL), eof(false), failed(false), lastRead(0) {}
	~Bz2Reader(void)
	{
		int err;
		if (bz)
			BZ2_bzReadClose(&err, bz);
		if (fp)
			fclose(fp);
	}
	bool Open(const std::wstring &fileName)
	{
		fp = _wfopen(fileName.c_str(), L"rb");
		if (fp == NULL)
			return false;
		int err;
		bz = BZ2_bzReadOpen(&err, fp, 0, 0, NULL, 0);
		return err == BZ_OK;
	}
	/** \return false on error or end of data before len bytes (see Failed(), LastRead()) */
	bool Read(char *buf, int len)
	{
		lastRead = 0;
		while (len > 0)
		{
			if (eof || failed)
				return false;
			int err;
			int n = BZ2_bzRead(&err, bz, buf, len);
			if (err != BZ_OK && err != BZ_STREAM_END)
			{
				failed = true;	// corrupted / truncated data
				return false;
			}
			if (err == BZ_STREAM_END)
				eof = true;
			buf += n;
			len -= n;
			lastRead += n;
		}
		return true;
	}
	bool Failed(void) const { return failed; }
	/** bytes read by last Read() call */
	int LastRead(void) const { return lastRead; }
private:
	FILE *fp;
	BZFILE *bz;
	bool eof;
	bool failed;
	int lastRead;
};

unsigned long long ParseOctal(const char *p, int len)
{
	unsigned long long val = 0;
	if (static_cast<unsigned char>(p[0]) & 0x80)
	{
		// base-256 (GNU) for large values
		for (int i = 1; i < len; i++)
			val = (val << 8) | static_cast<unsigned char>(p[i]);
		return val;
	}
	for (int i = 0; i < len && p[i]; i++)
	{
		if (p[i] >= '0' && p[i] <= '7')
			val = val * 8 + (p[i] - '0');
	}
	return val;
}

std::string FieldString(const char *p, int len)
{
	int n = 0;
	while (n < len && p[n])
		n++;
	return std::string(p, n);
}

/** Validate path from archive: relative, no "..", no drive / stream separators */
bool IsReservedName(const std::wstring &part)
{
	// device names, also with extension (e.g. "CON.txt")
	std::wstring base = part.substr(0, part.find(L'.'));
	for (size_t i = 0; i < base.size(); i++)
		base[i] = towupper(base[i]);
	static const wchar_t* const names[] = { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" };
	for (unsigned int i = 0; i < sizeof(names)/sizeof(names[0]); i++)
	{
		if (base == names[i])
			return true;
	}
	if (base.size() == 4 && (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0) &&
		((base[3] >= L'0' && base[3] <= L'9') || base[3] == 0xB9 || base[3] == 0xB2 || base[3] == 0xB3))
		return true;
	return false;
}

/** Validate and normalize path from archive: relative, "." components removed; rejected: "..",
	drive / stream separators, characters invalid in Windows file names, names ending with '.' or ' ',
	device names
*/
bool SafeRelativePath(const std::string &path, std::wstring &out)
{
	if (path.empty() || path.find('\0') != std::string::npos)
		return false;
	std::wstring w = Utils::Utf8ToWide(path);
	for (size_t i = 0; i < w.size(); i++)
	{
		if (w[i] == L'/')
			w[i] = L'\\';
	}
	if (w.empty() || w[0] == L'\\')
		return false;
	std::wstring result;
	size_t start = 0;
	for (;;)
	{
		size_t end = w.find(L'\\', start);
		std::wstring part = w.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
		if (!part.empty() && part != L".")
		{
			if (part == L".." || part[part.size() - 1] == L'.' || part[part.size() - 1] == L' ' || IsReservedName(part))
				return false;
			for (size_t i = 0; i < part.size(); i++)
			{
				if (part[i] < 0x20 || wcschr(L"<>:\"|?*", part[i]) != NULL)
					return false;
			}
			if (!result.empty())
				result += L'\\';
			result += part;
		}
		if (end == std::wstring::npos)
			break;
		start = end + 1;
	}
	if (result.empty())
		return false;
	out = result;
	return true;
}

bool HeaderChecksumOk(const char *hdr)
{
	unsigned long long stored = ParseOctal(hdr + 148, 8);
	unsigned long sum = 0;
	for (int i = 0; i < 512; i++)
		sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(hdr[i]);
	return stored == sum;
}

/** Value of "path" from pax extended header data */
std::string PaxPath(const std::string &data)
{
	size_t pos = 0;
	while (pos < data.size())
	{
		size_t space = data.find(' ', pos);
		if (space == std::string::npos)
			break;
		unsigned long len = strtoul(data.c_str() + pos, NULL, 10);
		// record: "<len> <key>=<value>\n", len includes everything
		if (len == 0 || len > data.size() - pos || space + 1 >= pos + len || data[pos + len - 1] != '\n')
			break;
		std::string record = data.substr(space + 1, pos + len - space - 2);	// without trailing '\n'
		if (record.compare(0, 5, "path=") == 0)
			return record.substr(5);
		pos += len;
	}
	return std::string();
}

}	// namespace

int VoiceDownload::FetchCatalog(std::vector<Asset> &assets, volatile long *cancel, std::string &error)
{
	assets.clear();
	std::string json;
	unsigned long long len;
	if (HttpGet(CATALOG_URL, StringSink, &json, len, cancel, error) != 0)
		return 1;
	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(json, root) || !root.isObject() || !root["assets"].isArray())
	{
		error = "unexpected reply from GitHub API";
		return 1;
	}
	const Json::Value &ja = root["assets"];
	for (unsigned int i = 0; i < ja.size(); i++)
	{
		const Json::Value &a = ja[i];
		if (!a.isObject() || !a["name"].isString() || !a["browser_download_url"].isString())
			continue;
		Asset asset;
		asset.name = a["name"].asString();
		if (!IsSupportedArchive(asset.name) || asset.name.find_first_of("\\/:") != std::string::npos)
			continue;
		asset.url = a["browser_download_url"].asString();
		if (asset.url.compare(0, 19, "https://github.com/") != 0)
			continue;
		if (a["size"].isNumeric())
			asset.size = static_cast<unsigned long long>(a["size"].asDouble());
		assets.push_back(asset);
	}
	std::sort(assets.begin(), assets.end(), AssetLess);
	if (assets.empty())
	{
		error = "no voices found in GitHub reply";
		return 1;
	}
	return 0;
}

int VoiceDownload::Download(const std::string &url, const std::wstring &fileName, ProgressFn progress, void *arg,
	volatile long *cancel, std::string &error)
{
	FileSink fs;
	fs.fp = _wfopen(fileName.c_str(), L"wb");
	if (fs.fp == NULL)
	{
		error = "cannot create file " + Utils::WideToUtf8(fileName);
		return 1;
	}
	unsigned long long len = 0;
	fs.done = 0;
	fs.total = &len;
	fs.progress = progress;
	fs.arg = arg;
	int status = HttpGet(url, FileSinkFn, &fs, len, cancel, error);
	bool closeOk = fclose(fs.fp) == 0;
	if (status == 0 && !closeOk)
	{
		error = "failed to write " + Utils::WideToUtf8(fileName);
		status = 1;
	}
	if (status == 0 && len != 0 && fs.done != len)
	{
		error = "incomplete download";
		status = 1;
	}
	if (status != 0)
		DeleteFileW(fileName.c_str());
	return status;
}

namespace
{

/** Extract archive into destDir (no cleanup on error) */
int ExtractRaw(const std::wstring &archive, const std::wstring &destDir, std::wstring &topDir,
	ProgressFn progress, void *arg, volatile long *cancel, std::string &error)
{
	topDir.clear();
	Bz2Reader reader;
	if (!reader.Open(archive))
	{
		error = "cannot open archive " + Utils::WideToUtf8(archive);
		return 1;
	}
	if (!CreateDirectories(destDir))
	{
		error = "cannot create directory " + Utils::WideToUtf8(destDir);
		return 1;
	}

	unsigned long long written = 0;
	std::string longName;
	std::vector<char> buf(64 * 1024);
	int zeroBlocks = 0;
	for (;;)
	{
		if (cancel && *cancel)
		{
			error = "cancelled";
			return 2;
		}
		char hdr[512];
		if (!reader.Read(hdr, sizeof(hdr)))
		{
			// some archives end without (second) zero block: accept clean end of compressed stream
			// at header boundary, but not decompression error or partial header
			if (!reader.Failed() && reader.LastRead() == 0 && (zeroBlocks > 0 || !topDir.empty()))
				break;
			error = "corrupted or truncated archive";
			return 1;
		}
		bool allZero = true;
		for (int i = 0; i < 512; i++)
		{
			if (hdr[i])
			{
				allZero = false;
				break;
			}
		}
		if (allZero)
		{
			if (++zeroBlocks >= 2)
				break;
			continue;
		}
		zeroBlocks = 0;
		if (!HeaderChecksumOk(hdr))
		{
			error = "corrupted archive (header checksum) or not a tar.bz2 file";
			return 1;
		}

		unsigned long long size = ParseOctal(hdr + 124, 12);
		char type = hdr[156];
		std::string name = FieldString(hdr, 100);
		if (memcmp(hdr + 257, "ustar\0", 6) == 0)	// POSIX ustar: prefix field (old GNU format: other data there)
		{
			std::string prefix = FieldString(hdr + 345, 155);
			if (!prefix.empty())
				name = prefix + "/" + name;
		}
		if (!longName.empty())
		{
			name = longName;
			longName.clear();
		}
		unsigned long long padded = (size + 511) & ~511ULL;

		if (type == 'L' || type == 'x')
		{
			// GNU long name / pax extended header: name of next entry
			if (size > 64 * 1024)
			{
				error = "corrupted archive (long header)";
				return 1;
			}
			std::string data(static_cast<size_t>(padded), '\0');
			if (padded && !reader.Read(&data[0], static_cast<int>(padded)))
			{
				error = "corrupted archive";
				return 1;
			}
			data.resize(static_cast<size_t>(size));
			if (type == 'L')
				longName = FieldString(data.c_str(), static_cast<int>(data.size()));
			else
				longName = PaxPath(data);
			continue;
		}

		std::wstring rel;
		bool isFile = (type == '0' || type == '\0' || type == '7');
		bool isDir = (type == '5');
		if ((isFile || isDir) && !SafeRelativePath(name, rel))
		{
			error = "archive contains unsafe path: " + name;
			return 1;
		}
		if (isFile || isDir)
		{
			std::wstring first = rel.substr(0, rel.find(L'\\'));
			if (topDir.empty())
				topDir = first;
			else if (first != topDir)
			{
				error = "unexpected archive layout (more than one top directory)";
				return 1;
			}
		}

		if (isDir)
		{
			if (!CreateDirectories(destDir + L"\\" + rel))
			{
				error = "cannot create directory " + Utils::WideToUtf8(rel);
				return 1;
			}
		}
		if (!isFile)
		{
			// skip data of other entries (symlinks, global headers, ...)
			unsigned long long left = padded;
			while (left > 0)
			{
				int n = static_cast<int>(left > buf.size() ? buf.size() : left);
				if (!reader.Read(&buf[0], n))
				{
					error = "corrupted archive";
					return 1;
				}
				left -= n;
			}
			continue;
		}

		std::wstring full = destDir + L"\\" + rel;
		size_t slash = full.find_last_of(L'\\');
		if (!CreateDirectories(full.substr(0, slash)))
		{
			error = "cannot create directory for " + Utils::WideToUtf8(rel);
			return 1;
		}
		FILE *fp = _wfopen(full.c_str(), L"wb");
		if (fp == NULL)
		{
			error = "cannot create file " + Utils::WideToUtf8(full);
			return 1;
		}
		unsigned long long left = padded;
		unsigned long long toWrite = size;
		bool ok = true;
		while (left > 0 && ok)
		{
			int n = static_cast<int>(left > buf.size() ? buf.size() : left);
			if (!reader.Read(&buf[0], n))
			{
				error = "corrupted archive";
				ok = false;
				break;
			}
			size_t w = static_cast<size_t>(toWrite > static_cast<unsigned long long>(n) ? n : toWrite);
			if (w && fwrite(&buf[0], 1, w, fp) != w)
			{
				error = "failed to write " + Utils::WideToUtf8(full);
				ok = false;
			}
			toWrite -= w;
			left -= n;
			written += w;
			if (progress)
				progress(arg, written, 0);
			if (cancel && *cancel)
			{
				error = "cancelled";
				fclose(fp);
				return 2;
			}
		}
		if (fclose(fp) != 0 && ok)
		{
			error = "failed to write " + Utils::WideToUtf8(full);
			ok = false;
		}
		if (!ok)
			return 1;
	}
	if (topDir.empty())
	{
		error = "empty archive";
		return 1;
	}
	return 0;
}

}	// namespace

int VoiceDownload::ExtractTarBz2(const std::wstring &archive, const std::wstring &destDir, std::wstring &topDir,
	ProgressFn progress, void *arg, volatile long *cancel, std::string &error)
{
	// extract to temporary directory first: failed / cancelled installation does not touch existing voice
	std::wstring tmpDir = destDir + L"\\.install-tmp";
	RemoveDirectoryRecursive(tmpDir);
	int status = ExtractRaw(archive, tmpDir, topDir, progress, arg, cancel, error);
	if (status != 0)
	{
		RemoveDirectoryRecursive(tmpDir);
		topDir.clear();
		return status;
	}
	std::wstring target = destDir + L"\\" + topDir;
	std::wstring oldDir = destDir + L"\\.install-old";
	bool replaced = false;
	if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		// reinstallation: keep previous version until new one is in place
		RemoveDirectoryRecursive(oldDir);
		if (!MoveFileW(target.c_str(), oldDir.c_str()))
		{
			error = "cannot replace existing " + Utils::WideToUtf8(target) + " (files in use?)";
			RemoveDirectoryRecursive(tmpDir);
			topDir.clear();
			return 1;
		}
		replaced = true;
	}
	if (!MoveFileW((tmpDir + L"\\" + topDir).c_str(), target.c_str()))
	{
		error = "cannot move extracted files to " + Utils::WideToUtf8(target);
		if (replaced)
			MoveFileW(oldDir.c_str(), target.c_str());
		RemoveDirectoryRecursive(tmpDir);
		topDir.clear();
		return 1;
	}
	if (replaced)
		RemoveDirectoryRecursive(oldDir);
	RemoveDirectoryRecursive(tmpDir);
	return 0;
}

void VoiceDownload::RemoveDirectoryRecursive(const std::wstring &dir)
{
	if (dir.size() < 4)
		return;	// never e.g. "C:\"
	DWORD attr = GetFileAttributesW(dir.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
		return;
	if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
	{
		RemoveDirectoryW(dir.c_str());	// junction / symlink: remove link only, not target contents
		return;
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..")
				continue;
			std::wstring full = dir + L"\\" + name;
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				RemoveDirectoryRecursive(full);
			else
			{
				SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(full.c_str());
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}
