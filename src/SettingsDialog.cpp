#include "SettingsDialog.h"
#include "DownloadDialog.h"
#include "Voices.h"
#include "Utils.h"
#include "resource.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

namespace
{

const char* const MODEL_TYPES[] = { "vits", "kokoro", "kitten" };

struct DialogState
{
	HINSTANCE hInstance;
	Settings *settings;
	std::wstring baseDir;
	int curVoice;
	bool loading;
	std::vector<VoiceConfig> installed;	///< voices installed by download dialog - kept also if settings are cancelled
};

std::wstring GetText(HWND hDlg, int id)
{
	HWND h = GetDlgItem(hDlg, id);
	int len = GetWindowTextLengthW(h);
	std::vector<wchar_t> buf(len + 1);
	GetWindowTextW(h, &buf[0], len + 1);
	return std::wstring(&buf[0]);
}

std::string GetTextUtf8(HWND hDlg, int id)
{
	return Utils::WideToUtf8(Utils::Trim(GetText(hDlg, id)));
}

void SetTextUtf8(HWND hDlg, int id, const std::string &text)
{
	SetDlgItemTextW(hDlg, id, Utils::Utf8ToWide(text).c_str());
}

void SetInt(HWND hDlg, int id, int value)
{
	wchar_t buf[32];
	swprintf(buf, sizeof(buf)/sizeof(buf[0]), L"%d", value);
	SetDlgItemTextW(hDlg, id, buf);
}

int GetInt(HWND hDlg, int id, int def)
{
	std::wstring text = Utils::Trim(GetText(hDlg, id));
	if (text.empty())
		return def;
	return _wtoi(text.c_str());
}

std::wstring VoiceListName(const std::wstring &baseDir, const VoiceConfig &v)
{
	std::wstring name = Utils::Utf8ToWide(v.name);
	if (name.empty())
		name = L"(unnamed)";
	if (!Voices::IsInstalled(baseDir, v))
		name += L" (missing)";
	return name;
}

const int VOICE_FIELDS[] = {
	IDC_VOICE_NAME, IDC_VOICE_TYPE, IDC_VOICE_FILL, IDC_VOICE_MODEL, IDC_VOICE_MODEL_BROWSE, IDC_VOICE_TOKENS,
	IDC_VOICE_TOKENS_BROWSE, IDC_VOICE_DATA_DIR, IDC_VOICE_DATA_DIR_BROWSE, IDC_VOICE_LEXICON, IDC_VOICE_LEXICON_BROWSE,
	IDC_VOICE_VOICES, IDC_VOICE_VOICES_BROWSE, IDC_VOICE_LANG, IDC_VOICE_SPEAKER, IDC_VOICE_SPEED, IDC_VOICE_REMOVE
};

void LoadVoice(HWND hDlg, DialogState *st)
{
	st->loading = true;
	bool valid = st->curVoice >= 0 && st->curVoice < static_cast<int>(st->settings->voices.size());
	for (unsigned int i = 0; i < sizeof(VOICE_FIELDS)/sizeof(VOICE_FIELDS[0]); i++)
		EnableWindow(GetDlgItem(hDlg, VOICE_FIELDS[i]), valid);
	VoiceConfig empty;
	const VoiceConfig &v = valid ? st->settings->voices[st->curVoice] : empty;
	SetTextUtf8(hDlg, IDC_VOICE_NAME, valid ? v.name : "");
	int typeIndex = 0;
	for (unsigned int i = 0; i < sizeof(MODEL_TYPES)/sizeof(MODEL_TYPES[0]); i++)
	{
		if (v.modelType == MODEL_TYPES[i])
			typeIndex = i;
	}
	SendDlgItemMessageW(hDlg, IDC_VOICE_TYPE, CB_SETCURSEL, typeIndex, 0);
	SetTextUtf8(hDlg, IDC_VOICE_MODEL, v.model);
	SetTextUtf8(hDlg, IDC_VOICE_TOKENS, v.tokens);
	SetTextUtf8(hDlg, IDC_VOICE_DATA_DIR, v.dataDir);
	SetTextUtf8(hDlg, IDC_VOICE_LEXICON, v.lexicon);
	SetTextUtf8(hDlg, IDC_VOICE_VOICES, v.voices);
	SetTextUtf8(hDlg, IDC_VOICE_LANG, v.lang);
	SetInt(hDlg, IDC_VOICE_SPEAKER, v.speakerId);
	char buf[32];
	snprintf(buf, sizeof(buf), "%.2f", v.speed);
	SetTextUtf8(hDlg, IDC_VOICE_SPEED, buf);
	st->loading = false;
}

void SaveVoice(HWND hDlg, DialogState *st)
{
	if (st->curVoice < 0 || st->curVoice >= static_cast<int>(st->settings->voices.size()))
		return;
	VoiceConfig &v = st->settings->voices[st->curVoice];
	v.name = GetTextUtf8(hDlg, IDC_VOICE_NAME);
	int typeIndex = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_VOICE_TYPE, CB_GETCURSEL, 0, 0));
	if (typeIndex >= 0 && typeIndex < static_cast<int>(sizeof(MODEL_TYPES)/sizeof(MODEL_TYPES[0])))
		v.modelType = MODEL_TYPES[typeIndex];
	v.model = GetTextUtf8(hDlg, IDC_VOICE_MODEL);
	v.tokens = GetTextUtf8(hDlg, IDC_VOICE_TOKENS);
	v.dataDir = GetTextUtf8(hDlg, IDC_VOICE_DATA_DIR);
	v.lexicon = GetTextUtf8(hDlg, IDC_VOICE_LEXICON);
	v.voices = GetTextUtf8(hDlg, IDC_VOICE_VOICES);
	v.lang = GetTextUtf8(hDlg, IDC_VOICE_LANG);
	v.speakerId = GetInt(hDlg, IDC_VOICE_SPEAKER, 0);
	if (v.speakerId < 0)
		v.speakerId = 0;
	std::wstring speed = Utils::Trim(GetText(hDlg, IDC_VOICE_SPEED));
	for (size_t i = 0; i < speed.size(); i++)
	{
		if (speed[i] == L',')
			speed[i] = L'.';
	}
	double val = wcstod(speed.c_str(), NULL);
	if (val >= 0.25 && val <= 4.0)
		v.speed = val;
}

void FillVoiceList(HWND hDlg, DialogState *st)
{
	HWND hList = GetDlgItem(hDlg, IDC_VOICE_LIST);
	SendMessageW(hList, LB_RESETCONTENT, 0, 0);
	for (unsigned int i = 0; i < st->settings->voices.size(); i++)
		SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(VoiceListName(st->baseDir, st->settings->voices[i]).c_str()));
	SendMessageW(hList, LB_SETCURSEL, st->curVoice, 0);
}

std::wstring DirOf(const std::wstring &path)
{
	size_t pos = path.find_last_of(L"\\/");
	return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

void BrowseFile(HWND hDlg, DialogState *st, int editId, const wchar_t *filter)
{
	std::wstring current = Utils::ResolvePath(st->baseDir, Utils::Trim(GetText(hDlg, editId)));
	std::wstring initialDir = current.empty() ? st->baseDir : DirOf(current);
	if (!Utils::DirectoryExists(initialDir))
		initialDir = st->baseDir;

	wchar_t file[MAX_PATH * 2] = L"";
	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hDlg;
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = file;
	ofn.nMaxFile = sizeof(file)/sizeof(file[0]);
	ofn.lpstrInitialDir = initialDir.c_str();
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if (GetOpenFileNameW(&ofn))
		SetDlgItemTextW(hDlg, editId, Voices::MakeRelative(st->baseDir, file).c_str());
}

bool BrowseFolder(HWND hDlg, const wchar_t *title, std::wstring &result)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BROWSEINFOW bi;
	memset(&bi, 0, sizeof(bi));
	bi.hwndOwner = hDlg;
	bi.lpszTitle = title;
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	bool ok = false;
	PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
	if (pidl)
	{
		wchar_t path[MAX_PATH];
		if (SHGetPathFromIDListW(pidl, path))
		{
			result = path;
			ok = true;
		}
		CoTaskMemFree(pidl);
	}
	if (SUCCEEDED(hr))
		CoUninitialize();
	return ok;
}

void UpdateListName(HWND hDlg, DialogState *st)
{
	if (st->loading || st->curVoice < 0)
		return;
	std::wstring name = Utils::Trim(GetText(hDlg, IDC_VOICE_NAME));
	if (name.empty())
		name = L"(unnamed)";
	if (st->curVoice < static_cast<int>(st->settings->voices.size()) && !Voices::IsInstalled(st->baseDir, st->settings->voices[st->curVoice]))
		name += L" (missing)";
	HWND hList = GetDlgItem(hDlg, IDC_VOICE_LIST);
	SendMessageW(hList, LB_DELETESTRING, st->curVoice, 0);
	SendMessageW(hList, LB_INSERTSTRING, st->curVoice, reinterpret_cast<LPARAM>(name.c_str()));
	SendMessageW(hList, LB_SETCURSEL, st->curVoice, 0);
}

/** Fill fields of current voice from model directory selected by user */
void FillFromDirectory(HWND hDlg, DialogState *st)
{
	std::wstring dir;
	if (!BrowseFolder(hDlg, L"Select model directory (extracted sherpa-onnx TTS model archive)", dir))
		return;
	VoiceConfig v;
	if (!Voices::MakeVoiceFromDir(st->baseDir, dir, v))
	{
		MessageBoxW(hDlg, L"No .onnx model file found in selected directory.", L"Settings", MB_ICONWARNING);
		return;
	}
	std::wstring currentName = Utils::Trim(GetText(hDlg, IDC_VOICE_NAME));
	if (!currentName.empty() && currentName != L"New voice")
		v.name = Utils::WideToUtf8(currentName);
	v.speakerId = GetInt(hDlg, IDC_VOICE_SPEAKER, 0);
	st->settings->voices[st->curVoice] = v;
	LoadVoice(hDlg, st);
	UpdateListName(hDlg, st);
}

/** Download dialog: installed voices are added to the list */
void DownloadVoices(HWND hDlg, DialogState *st)
{
	SaveVoice(hDlg, st);
	std::vector<std::wstring> dirs;
	DownloadDialog::Show(st->hInstance, hDlg, st->baseDir, dirs);
	for (unsigned int i = 0; i < dirs.size(); i++)
	{
		VoiceConfig v;
		if (!Voices::MakeVoiceFromDir(st->baseDir, dirs[i], v))
			continue;
		bool exists = false;
		for (unsigned int j = 0; j < st->settings->voices.size(); j++)
		{
			if (st->settings->voices[j].model == v.model)
			{
				exists = true;	// voice re-downloaded: keep its settings
				st->curVoice = j;
			}
		}
		if (!exists)
		{
			st->installed.push_back(v);
			st->settings->voices.push_back(v);
			st->curVoice = static_cast<int>(st->settings->voices.size()) - 1;
		}
	}
	FillVoiceList(hDlg, st);
	LoadVoice(hDlg, st);
}

void InitDialog(HWND hDlg, DialogState *st)
{
	const Settings &s = *st->settings;
	CheckDlgButton(hDlg, IDC_ALWAYS_ON_TOP, s.wnd.alwaysOnTop ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_SEND_ON_ENTER, s.sendOnEnter ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_SILENCE_MIC, s.silenceMicrophone ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_SHOW_VOICE, s.showVoiceInHistory ? BST_CHECKED : BST_UNCHECKED);
	SetInt(hDlg, IDC_EMBED_BUTTON_ID, s.embedButtonId);
	SetInt(hDlg, IDC_NUM_THREADS, s.numThreads);
	SetTextUtf8(hDlg, IDC_LIBRARY_DIR, s.libraryDir);
	SetDlgItemTextW(hDlg, IDC_BASE_DIR_INFO, (L"Plugin data directory: " + st->baseDir).c_str());
	for (unsigned int i = 0; i < sizeof(MODEL_TYPES)/sizeof(MODEL_TYPES[0]); i++)
		SendDlgItemMessageW(hDlg, IDC_VOICE_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utils::Utf8ToWide(MODEL_TYPES[i]).c_str()));
	if (st->settings->voices.empty())
	{
		// e.g. settings shown before first start: offer voices present in data directory
		st->settings->voices = Voices::FindInstalled(st->baseDir);
		st->settings->currentVoice = 0;
	}
	st->curVoice = s.voices.empty() ? -1 : s.currentVoice;
	FillVoiceList(hDlg, st);
	LoadVoice(hDlg, st);
}

bool Apply(HWND hDlg, DialogState *st)
{
	SaveVoice(hDlg, st);
	Settings &s = *st->settings;
	s.wnd.alwaysOnTop = IsDlgButtonChecked(hDlg, IDC_ALWAYS_ON_TOP) == BST_CHECKED;
	s.sendOnEnter = IsDlgButtonChecked(hDlg, IDC_SEND_ON_ENTER) == BST_CHECKED;
	s.silenceMicrophone = IsDlgButtonChecked(hDlg, IDC_SILENCE_MIC) == BST_CHECKED;
	s.showVoiceInHistory = IsDlgButtonChecked(hDlg, IDC_SHOW_VOICE) == BST_CHECKED;
	s.embedButtonId = GetInt(hDlg, IDC_EMBED_BUTTON_ID, 0);
	if (s.embedButtonId < 0)
		s.embedButtonId = 0;
	int threads = GetInt(hDlg, IDC_NUM_THREADS, 2);
	if (threads < 1 || threads > 16)
	{
		MessageBoxW(hDlg, L"Number of threads: 1...16", L"Settings", MB_ICONWARNING);
		return false;
	}
	s.numThreads = threads;
	s.libraryDir = GetTextUtf8(hDlg, IDC_LIBRARY_DIR);
	if (st->curVoice >= 0)
		s.currentVoice = st->curVoice;
	if (s.currentVoice >= static_cast<int>(s.voices.size()))
		s.currentVoice = 0;
	return true;
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	DialogState *st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hDlg, DWLP_USER));
	switch (msg)
	{
	case WM_INITDIALOG:
		st = reinterpret_cast<DialogState*>(lParam);
		SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
		InitDialog(hDlg, st);
		return TRUE;
	case WM_COMMAND:
	{
		int id = LOWORD(wParam);
		int code = HIWORD(wParam);
		switch (id)
		{
		case IDOK:
			if (Apply(hDlg, st))
				EndDialog(hDlg, IDOK);
			return TRUE;
		case IDCANCEL:
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		case IDC_VOICE_LIST:
			if (code == LBN_SELCHANGE)
			{
				int sel = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_VOICE_LIST, LB_GETCURSEL, 0, 0));
				if (sel != st->curVoice)
				{
					SaveVoice(hDlg, st);
					st->curVoice = sel;
					LoadVoice(hDlg, st);
				}
			}
			return TRUE;
		case IDC_VOICE_NAME:
			if (code == EN_CHANGE)
				UpdateListName(hDlg, st);
			return TRUE;
		case IDC_VOICE_ADD:
		{
			SaveVoice(hDlg, st);
			VoiceConfig v;
			v.name = "New voice";
			st->settings->voices.push_back(v);
			st->curVoice = static_cast<int>(st->settings->voices.size()) - 1;
			FillVoiceList(hDlg, st);
			LoadVoice(hDlg, st);
			SetFocus(GetDlgItem(hDlg, IDC_VOICE_NAME));
			SendDlgItemMessageW(hDlg, IDC_VOICE_NAME, EM_SETSEL, 0, -1);
			return TRUE;
		}
		case IDC_VOICE_REMOVE:
		{
			std::vector<VoiceConfig> &voices = st->settings->voices;
			if (st->curVoice < 0 || st->curVoice >= static_cast<int>(voices.size()))
				return TRUE;
			voices.erase(voices.begin() + st->curVoice);
			int &cur = st->settings->currentVoice;
			if (cur > st->curVoice || cur >= static_cast<int>(voices.size()))
				cur = cur > 0 ? cur - 1 : 0;
			if (st->curVoice >= static_cast<int>(voices.size()))
				st->curVoice = static_cast<int>(voices.size()) - 1;
			FillVoiceList(hDlg, st);
			LoadVoice(hDlg, st);
			return TRUE;
		}
		case IDC_VOICE_FILL:
			FillFromDirectory(hDlg, st);
			return TRUE;
		case IDC_VOICE_DOWNLOAD:
			DownloadVoices(hDlg, st);
			return TRUE;
		case IDC_VOICE_MODEL_BROWSE:
			BrowseFile(hDlg, st, IDC_VOICE_MODEL, L"ONNX model (*.onnx)\0*.onnx\0All files\0*.*\0");
			return TRUE;
		case IDC_VOICE_TOKENS_BROWSE:
			BrowseFile(hDlg, st, IDC_VOICE_TOKENS, L"Tokens (*.txt)\0*.txt\0All files\0*.*\0");
			return TRUE;
		case IDC_VOICE_LEXICON_BROWSE:
			BrowseFile(hDlg, st, IDC_VOICE_LEXICON, L"Lexicon (*.txt)\0*.txt\0All files\0*.*\0");
			return TRUE;
		case IDC_VOICE_VOICES_BROWSE:
			BrowseFile(hDlg, st, IDC_VOICE_VOICES, L"Voices (*.bin)\0*.bin\0All files\0*.*\0");
			return TRUE;
		case IDC_VOICE_DATA_DIR_BROWSE:
		{
			std::wstring dir;
			if (BrowseFolder(hDlg, L"Select espeak-ng-data directory", dir))
				SetDlgItemTextW(hDlg, IDC_VOICE_DATA_DIR, Voices::MakeRelative(st->baseDir, dir).c_str());
			return TRUE;
		}
		case IDC_LIBRARY_DIR_BROWSE:
		{
			std::wstring dir;
			if (BrowseFolder(hDlg, L"Select directory with sherpa-onnx-c-api.dll and onnxruntime.dll", dir))
				SetDlgItemTextW(hDlg, IDC_LIBRARY_DIR, Voices::MakeRelative(st->baseDir, dir).c_str());
			return TRUE;
		}
		default:
			break;
		}
		break;
	}
	default:
		break;
	}
	return FALSE;
}

}	// namespace

bool SettingsDialog::Show(HINSTANCE hInstance, HWND parent, Settings &settings, const std::wstring &baseDir)
{
	Settings edited = settings;
	DialogState st;
	st.hInstance = hInstance;
	st.settings = &edited;
	st.baseDir = baseDir;
	st.curVoice = -1;
	st.loading = false;
	INT_PTR ret = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), parent, DlgProc, reinterpret_cast<LPARAM>(&st));
	if (ret != IDOK)
	{
		// cancelled: other changes are discarded, but newly installed voices (files are already there) are added
		if (st.installed.empty())
			return false;
		for (unsigned int i = 0; i < st.installed.size(); i++)
		{
			bool exists = false;
			for (unsigned int j = 0; j < settings.voices.size(); j++)
			{
				if (settings.voices[j].model == st.installed[i].model)
					exists = true;
			}
			if (!exists)
				settings.voices.push_back(st.installed[i]);
		}
		return true;
	}
	settings = edited;
	return true;
}
