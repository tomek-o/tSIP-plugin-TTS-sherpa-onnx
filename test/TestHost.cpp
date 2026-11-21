/** \file
	\brief Test host: loads plugin DLL like tSIP (interface v2) with fake Invoke() results and drives its window

	Usage: TestHost.exe <plugin.dll> <profileDir> <mode: call|nocall|embed> <text1> <text2 (2nd voice)> [screenshot.bmp]

	- profileDir must contain plugin config (<dll name>.cfg) with working voices (2 voices for full test)
	- call:   PlayAudioFile succeeds (call uid 1), end of file reported after WAV duration
	- nocall: PlayAudioFile returns TSIP_ERR_NOT_AVAILABLE - plugin plays locally
	- embed:  like call, plugin window embedded in fake "button" window (RunScript GetButtonHandle)
	Exit code 0 if all checks passed.
*/
#include "tSIP_plugin_v2.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace
{

std::string mode;
std::wstring profileDir;
HWND hFakeButton = NULL;
int audioErrorCount = 0;
DWORD playEndTick = 0;
bool playing = false;
int playCount = 0;
int sidetoneCount = 0;
int restoreCount = 0;
int failures = 0;
std::string lastPlayedFile;

void Check(bool cond, const char *what)
{
	printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

char* Dup(const std::string &s)
{
	char *p = static_cast<char*>(malloc(s.size() + 1));
	memcpy(p, s.c_str(), s.size() + 1);
	return p;
}

std::string GetJsonString(const char *json, const char *key)
{
	std::string k = std::string("\"") + key + "\":\"";
	const char *p = strstr(json, k.c_str());
	if (!p)
		return "";
	p += k.size();
	const char *e = strchr(p, '"');
	return e ? std::string(p, e - p) : "";
}

DWORD WavDurationMs(const std::wstring &path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	unsigned char hdr[44];
	DWORD read = 0;
	ReadFile(h, hdr, sizeof(hdr), &read, NULL);
	CloseHandle(h);
	if (read < 44)
		return 0;
	unsigned int rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
	unsigned int size = hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24);
	return rate ? static_cast<DWORD>(size / 2ULL * 1000 / rate) : 0;
}

void TSIP_V2_CALL HostLog(void *, const char *text)
{
	printf("LOG: %s", text);
	fflush(stdout);
}

int TSIP_V2_CALL HostInvoke(void *, const char *method, const char *params, char **result)
{
	if (result)
		*result = NULL;
	std::string m = method;
	printf("INVOKE %s %s\n", method, params ? params : "");
	if (m == "GetAppInfo")
	{
		int len = WideCharToMultiByte(CP_UTF8, 0, profileDir.c_str(), -1, NULL, 0, NULL, NULL);
		std::string dir(len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, profileDir.c_str(), -1, &dir[0], len, NULL, NULL);
		std::string escaped;
		for (size_t i = 0; i < dir.size(); i++)
		{
			if (dir[i] == '\\')
				escaped += "\\\\";
			else
				escaped += dir[i];
		}
		if (result)
			*result = Dup("{\"exeName\":\"TestHost.exe\",\"profileDir\":\"" + escaped + "\",\"pluginInterface\":2}");
		return TSIP_OK;
	}
	if (m == "PlayAudioFile")
	{
		if (mode == "nocall")
			return TSIP_ERR_NOT_AVAILABLE;
		std::string file = GetJsonString(params, "file");
		std::wstring wfile(file.begin(), file.end());
		std::wstring full = profileDir + L"\\" + wfile;
		if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			if (result)
				*result = Dup("{\"error\":\"file not found\"}");
			return TSIP_ERR_NOT_AVAILABLE;
		}
		char buf[128];
		snprintf(buf, sizeof(buf), "{\"uid\":1,\"audioErrorCount\":%d,\"file\":\"x\"}", audioErrorCount);
		if (result)
			*result = Dup(buf);
		DWORD duration = WavDurationMs(full);
		printf("  playing %s, %lu ms\n", file.c_str(), duration);
		if (file.find("silence") == std::string::npos)
		{
			playCount++;
			lastPlayedFile = file;
		}
		playing = true;
		playEndTick = GetTickCount() + duration;
		return TSIP_OK;
	}
	if (m == "GetCallAudioState")
	{
		if (playing && GetTickCount() >= playEndTick)
		{
			playing = false;
			audioErrorCount++;
		}
		char buf[128];
		snprintf(buf, sizeof(buf), "{\"uid\":1,\"audioErrorCount\":%d,\"rxLevel\":0}", audioErrorCount);
		if (result)
			*result = Dup(buf);
		return TSIP_OK;
	}
	if (m == "SetAudioInput")
	{
		restoreCount++;
		return TSIP_OK;
	}
	if (m == "Sidetone")
	{
		sidetoneCount++;
		return mode == "nocall" ? TSIP_ERR_NOT_AVAILABLE : TSIP_OK;
	}
	if (m == "RunScript")
	{
		if (strstr(params, "GetButtonHandle") && hFakeButton)
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "{\"return\":\"%lu\"}", static_cast<unsigned long>(reinterpret_cast<UINT_PTR>(hFakeButton)));
			if (result)
				*result = Dup(buf);
			return TSIP_OK;
		}
		return TSIP_ERR_GENERIC;
	}
	return TSIP_ERR_UNKNOWN_METHOD;
}

void TSIP_V2_CALL HostFree(void *, void *ptr)
{
	free(ptr);
}

void Pump(DWORD ms)
{
	DWORD end = GetTickCount() + ms;
	while (GetTickCount() < end)
	{
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		Sleep(10);
	}
}

std::wstring GetText(HWND h)
{
	int len = GetWindowTextLengthW(h);
	std::vector<wchar_t> buf(len + 1);
	GetWindowTextW(h, &buf[0], len + 1);
	return std::wstring(&buf[0]);
}

std::wstring Utf8ToWide(const char *s)
{
	int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	std::vector<wchar_t> buf(len);
	MultiByteToWideChar(CP_UTF8, 0, s, -1, &buf[0], len);
	return std::wstring(&buf[0]);
}

/** wait until history contains text */
bool WaitForHistory(HWND hHistory, const std::wstring &text, DWORD timeoutMs)
{
	DWORD end = GetTickCount() + timeoutMs;
	while (GetTickCount() < end)
	{
		Pump(100);
		if (GetText(hHistory).find(text) != std::wstring::npos)
			return true;
	}
	return false;
}

int CountOccurrences(const std::wstring &haystack, const std::wstring &needle)
{
	int count = 0;
	for (size_t pos = haystack.find(needle); pos != std::wstring::npos; pos = haystack.find(needle, pos + 1))
		count++;
	return count;
}

void SaveScreenshot(HWND hwnd, const wchar_t *file)
{
	RECT rc;
	GetWindowRect(hwnd, &rc);
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	HDC hdcScreen = GetDC(NULL);
	HDC hdc = CreateCompatibleDC(hdcScreen);
	HBITMAP bmp = CreateCompatibleBitmap(hdcScreen, w, h);
	HGDIOBJ old = SelectObject(hdc, bmp);
	// copy from screen (PrintWindow does not work for all windows, e.g. under wine);
	// window is made topmost first so that only this window (not other applications) is captured
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetForegroundWindow(hwnd);
	if (GetWindowThreadProcessId(hwnd, NULL) == GetCurrentThreadId())
		Pump(500);
	else
		Sleep(500);
	BitBlt(hdc, 0, 0, w, h, hdcScreen, rc.left, rc.top, SRCCOPY);
	SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	BITMAPINFOHEADER bih;
	memset(&bih, 0, sizeof(bih));
	bih.biSize = sizeof(bih);
	bih.biWidth = w;
	bih.biHeight = h;
	bih.biPlanes = 1;
	bih.biBitCount = 24;
	int stride = ((w * 3 + 3) & ~3);
	std::vector<unsigned char> pixels(stride * h);
	GetDIBits(hdc, bmp, 0, h, &pixels[0], reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
	BITMAPFILEHEADER bfh;
	memset(&bfh, 0, sizeof(bfh));
	bfh.bfType = 0x4D42;
	bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
	bfh.bfSize = bfh.bfOffBits + static_cast<DWORD>(pixels.size());
	FILE *fp = _wfopen(file, L"wb");
	if (fp)
	{
		fwrite(&bfh, sizeof(bfh), 1, fp);
		fwrite(&bih, sizeof(bih), 1, fp);
		fwrite(&pixels[0], pixels.size(), 1, fp);
		fclose(fp);
	}
	SelectObject(hdc, old);
	DeleteObject(bmp);
	DeleteDC(hdc);
	ReleaseDC(NULL, hdcScreen);
}

/** Close dialog with given caption from other thread (modal dialog blocks test), optional screenshot */
struct DialogCloser
{
	const wchar_t *title;
	const wchar_t *screenshot;
	bool found;
	static DWORD WINAPI Proc(LPVOID arg)
	{
		DialogCloser *dc = static_cast<DialogCloser*>(arg);
		HWND hDlg = NULL;
		for (int i = 0; i < 100 && hDlg == NULL; i++)
		{
			Sleep(100);
			hDlg = FindWindowW(L"#32770", dc->title);
		}
		dc->found = (hDlg != NULL);
		if (hDlg)
		{
			Sleep(700);
			if (dc->screenshot)
				SaveScreenshot(hDlg, dc->screenshot);
			PostMessageW(hDlg, WM_COMMAND, IDCANCEL, 0);
		}
		return 0;
	}
};

LRESULT CALLBACK FakeButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}	// namespace

int wmain(int argc, wchar_t **argv)
{
	if (argc < 6)
	{
		fprintf(stderr, "Usage: TestHost <plugin.dll> <profileDir> <call|nocall|embed> <text1> <text2> [screenshot.bmp]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	profileDir = argv[2];
	for (wchar_t *p = argv[3]; *p; p++)
		mode += static_cast<char>(*p);

	// texts given as UTF-8 files to avoid command line code page problems
	std::wstring texts[2];
	for (int i = 0; i < 2; i++)
	{
		FILE *fp = _wfopen(argv[4 + i], L"rb");
		if (!fp)
		{
			fprintf(stderr, "cannot read text file\n");
			return 2;
		}
		char buf[4096] = "";
		size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
		buf[len] = 0;
		fclose(fp);
		texts[i] = Utf8ToWide(buf);
	}

	if (mode == "embed")
	{
		WNDCLASSW wc;
		memset(&wc, 0, sizeof(wc));
		wc.lpfnWndProc = FakeButtonProc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_HIGHLIGHT + 1);
		wc.lpszClassName = L"FakeButton";
		RegisterClassW(&wc);
		hFakeButton = CreateWindowW(L"FakeButton", L"fake tSIP button", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			100, 100, 460, 330, NULL, NULL, wc.hInstance, NULL);
	}

	HMODULE hDll = LoadLibraryW(argv[1]);
	if (!hDll)
	{
		fprintf(stderr, "cannot load plugin, error %lu\n", GetLastError());
		return 2;
	}
	pfTSipPluginInit init = reinterpret_cast<pfTSipPluginInit>(reinterpret_cast<void*>(GetProcAddress(hDll, TSIP_PLUGIN_INIT_NAME)));
	Check(init != NULL, "tSipPluginInit exported");
	if (!init)
		return 1;

	TSIP_HOST_API host;
	memset(&host, 0, sizeof(host));
	host.size = sizeof(host);
	host.cookie = &host;
	host.Log = HostLog;
	host.Invoke = HostInvoke;
	host.Free = HostFree;
	TSIP_PLUGIN_API plugin;
	memset(&plugin, 0, sizeof(plugin));
	plugin.size = sizeof(plugin);
	Check(init(&host, &plugin) == 0, "tSipPluginInit returns 0");
	Check(plugin.Start && plugin.Stop && plugin.Shutdown && plugin.OnEvent && plugin.ShowSettings && plugin.GetMetadata,
		"plugin functions set");
	printf("metadata: %s\n", plugin.GetMetadata());

	if (mode == "settings")
	{
		// settings dialog (modal, plugin not started): screenshot and close from another thread
		struct Closer
		{
			static DWORD WINAPI Proc(LPVOID arg)
			{
				HWND hDlg = NULL;
				for (int i = 0; i < 100 && hDlg == NULL; i++)
				{
					Sleep(100);
					hDlg = FindWindowW(L"#32770", L"Text-to-speech (sherpa-onnx) settings");
				}
				if (hDlg)
				{
					Sleep(700);
					SaveScreenshot(hDlg, static_cast<const wchar_t*>(arg));
					PostMessageW(hDlg, WM_COMMAND, IDCANCEL, 0);
				}
				return 0;
			}
		};
		HANDLE h = CreateThread(NULL, 0, Closer::Proc, argc > 6 ? argv[6] : const_cast<wchar_t*>(L"settings.bmp"), 0, NULL);
		plugin.ShowSettings(NULL);
		WaitForSingleObject(h, 15000);
		CloseHandle(h);
		plugin.Shutdown();
		FreeLibrary(hDll);
		printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
		return failures ? 1 : 0;
	}

	Check(plugin.Start() == 0, "Start");
	if (mode != "nocall")
		plugin.OnEvent("call_state", "{\"uid\":1,\"state\":6,\"stateName\":\"ESTABLISHED\",\"incoming\":false,\"peer\":\"sip:1@x\",\"peerName\":\"\"}");
	Pump(300);

	HWND hUi = (mode == "embed") ? FindWindowExW(hFakeButton, NULL, L"tSIP_TTS_sherpa_onnx_ui", NULL)
		: FindWindowW(L"tSIP_TTS_sherpa_onnx_ui", NULL);
	Check(hUi != NULL, "plugin window created");
	if (!hUi)
		return 1;
	if (mode == "embed")
	{
		RECT rcB, rcU;
		GetClientRect(hFakeButton, &rcB);
		GetClientRect(hUi, &rcU);
		Check(rcB.right == rcU.right && rcB.bottom == rcU.bottom, "embedded window fills button");
		MoveWindow(hFakeButton, 100, 100, 520, 380, TRUE);
		Pump(1200);
		GetClientRect(hFakeButton, &rcB);
		GetClientRect(hUi, &rcU);
		Check(rcB.right == rcU.right && rcB.bottom == rcU.bottom, "embedded window follows button size");
	}
	HWND hHistory = GetDlgItem(hUi, 103);
	HWND hInput = GetDlgItem(hUi, 107);
	HWND hSend = GetDlgItem(hUi, 108);
	HWND hRepeat = GetDlgItem(hUi, 105);
	HWND hVoice = GetDlgItem(hUi, 100);
	HWND hStatus = GetDlgItem(hUi, 101);

	// wait for preload of first voice: status "Loading..." cleared when voice is ready
	DWORD end = GetTickCount() + 60000;
	while (GetTickCount() < end && !GetText(hStatus).empty())
		Pump(100);
	Check(GetText(hStatus).empty(), "first voice loaded");

	// 1st message: typed + Enter
	SetWindowTextW(hInput, texts[0].c_str());
	SendMessageW(hInput, WM_KEYDOWN, VK_RETURN, 0);
	Check(GetText(hInput).empty(), "Enter sends (input cleared)");
	Check(WaitForHistory(hHistory, texts[0], 60000), "message 1 played and moved to history");

	// repeat
	SendMessageW(hRepeat, BM_CLICK, 0, 0);
	Pump(300);
	end = GetTickCount() + 30000;
	while (GetTickCount() < end && CountOccurrences(GetText(hHistory), texts[0]) < 2)
		Pump(100);
	Check(CountOccurrences(GetText(hHistory), texts[0]) >= 2, "Repeat plays last message again");

	// 2nd voice
	if (SendMessageW(hVoice, CB_GETCOUNT, 0, 0) >= 2)
	{
		SendMessageW(hVoice, CB_SETCURSEL, 1, 0);
		SendMessageW(hUi, WM_COMMAND, MAKEWPARAM(100, CBN_SELCHANGE), reinterpret_cast<LPARAM>(hVoice));
		SetWindowTextW(hInput, texts[1].c_str());
		SendMessageW(hSend, BM_CLICK, 0, 0);
		Check(WaitForHistory(hHistory, texts[1], 90000), "message 2 (second voice) played");
	}

	// quick text button 1 (text defined in config by test script, if any)
	{
		HWND hQuick = GetDlgItem(hUi, 130);
		Check(hQuick != NULL, "quick text buttons created");
		wchar_t caption[64] = L"";
		GetWindowTextW(hQuick, caption, 64);
		if (wcscmp(caption, L"+") != 0)
		{
			SendMessageW(hQuick, BM_CLICK, 0, 0);
			end = GetTickCount() + 60000;
			while (GetTickCount() < end && GetText(hHistory).find(L"Quick text test") == std::wstring::npos)
				Pump(100);
			Check(GetText(hHistory).find(L"Quick text test") != std::wstring::npos, "quick text button speaks its text");
		}
	}

	// empty quick text button opens edit dialog
	{
		DialogCloser dc;
		dc.title = L"Quick text button";
		dc.screenshot = argc > 7 ? argv[7] : NULL;
		dc.found = false;
		HANDLE h = CreateThread(NULL, 0, DialogCloser::Proc, &dc, 0, NULL);
		SendMessageW(GetDlgItem(hUi, 135), BM_CLICK, 0, 0);
		WaitForSingleObject(h, 15000);
		CloseHandle(h);
		Check(dc.found, "empty quick text button opens edit dialog");
	}

	// sidetone checkbox
	if (mode != "nocall")
	{
		int before = sidetoneCount;
		HWND hSidetone = GetDlgItem(hUi, 110);
		SendMessageW(hSidetone, BM_CLICK, 0, 0);
		Check(sidetoneCount == before + 1, "sidetone checkbox calls Sidetone");
		Check(playCount >= 3, "PlayAudioFile used for messages");
	}

	// stop during generation
	SetWindowTextW(hInput, L"This is a long sentence that will be stopped before it is played. And another one, also long enough.");
	SendMessageW(hSend, BM_CLICK, 0, 0);
	Pump(200);
	SendMessageW(GetDlgItem(hUi, 106), BM_CLICK, 0, 0);
	Pump(5000);
	Check(GetText(hHistory).find(L"This is a long sentence") == std::wstring::npos, "Stop cancels pending message");

	if (argc > 6)
		SaveScreenshot(mode == "embed" ? hFakeButton : hUi, argv[6]);

	SendMessageW(GetDlgItem(hUi, 112), BM_CLICK, 0, 0);
	Check(GetText(hHistory).empty(), "Clear button clears history");

	printf("history:\n%ls\n", GetText(hHistory).c_str());

	int restoreBefore = restoreCount;
	plugin.Stop();
	Check(FindWindowW(L"tSIP_TTS_sherpa_onnx_ui", NULL) == NULL, "window destroyed on Stop");
	if (mode != "nocall")
		Check(restoreCount > restoreBefore, "microphone restored on Stop");
	plugin.Shutdown();
	FreeLibrary(hDll);
	if (hFakeButton)
		DestroyWindow(hFakeButton);
	printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
