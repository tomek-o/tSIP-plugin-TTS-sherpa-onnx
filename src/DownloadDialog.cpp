#include "DownloadDialog.h"
#include "VoiceDownload.h"
#include "Utils.h"
#include "Log.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>

namespace
{

enum
{
	WM_APP_CATALOG = WM_APP + 10,	///< lParam = CatalogResult*
	WM_APP_PROGRESS,				///< wParam = percent (or -1), lParam = std::wstring* status
	WM_APP_ITEM_DONE,				///< lParam = ItemResult*
	WM_APP_JOB_DONE
};

struct CatalogResult
{
	std::vector<VoiceDownload::Asset> assets;
	std::string error;
};

struct ItemResult
{
	std::wstring name;
	std::wstring dir;		///< installed directory (full path)
	int status;
	std::string error;
};

struct Job
{
	enum Type { CATALOG, DOWNLOAD, LOCAL_FILE } type;
	HWND hDlg;
	std::wstring baseDir;
	std::vector<VoiceDownload::Asset> assets;
	std::wstring localFile;
	volatile long *cancel;
};

struct DialogState
{
	std::wstring baseDir;
	std::vector<std::wstring> *installedDirs;
	HWND hDlg;
	std::vector<VoiceDownload::Asset> catalog;
	std::vector<int> listToCatalog;		///< list box item -> catalog index
	HANDLE hThread;
	volatile long cancel;
	bool closeRequested;
	std::wstring installedNames;
};

struct ProgressCtx
{
	HWND hDlg;
	std::wstring prefix;
	int lastPercent;
	DWORD lastTick;
};

void PostProgress(HWND hDlg, int percent, const std::wstring &text)
{
	std::wstring *s = new std::wstring(text);
	if (!PostMessageW(hDlg, WM_APP_PROGRESS, static_cast<WPARAM>(percent), reinterpret_cast<LPARAM>(s)))
		delete s;
}

void DownloadProgress(void *arg, unsigned long long done, unsigned long long total)
{
	ProgressCtx *ctx = static_cast<ProgressCtx*>(arg);
	int percent = total ? static_cast<int>(done * 100 / total) : -1;
	DWORD now = GetTickCount();
	if (percent == ctx->lastPercent && now - ctx->lastTick < 500)
		return;
	ctx->lastPercent = percent;
	ctx->lastTick = now;
	wchar_t buf[96];
	if (total)
		swprintf(buf, sizeof(buf)/sizeof(buf[0]), L" %.1f / %.1f MB", done / 1048576.0, total / 1048576.0);
	else
		swprintf(buf, sizeof(buf)/sizeof(buf[0]), L" %.1f MB", done / 1048576.0);
	PostProgress(ctx->hDlg, percent, ctx->prefix + buf);
}

void ExtractProgress(void *arg, unsigned long long done, unsigned long long)
{
	ProgressCtx *ctx = static_cast<ProgressCtx*>(arg);
	DWORD now = GetTickCount();
	if (now - ctx->lastTick < 300)
		return;
	ctx->lastTick = now;
	wchar_t buf[64];
	swprintf(buf, sizeof(buf)/sizeof(buf[0]), L" %.1f MB", done / 1048576.0);
	PostProgress(ctx->hDlg, -1, ctx->prefix + buf);
}

void InstallArchive(Job *job, const std::wstring &archive, const std::wstring &displayName, ItemResult *r)
{
	ProgressCtx ctx;
	ctx.hDlg = job->hDlg;
	ctx.prefix = L"Extracting " + displayName + L"...";
	ctx.lastPercent = -2;
	ctx.lastTick = 0;
	std::wstring topDir;
	r->status = VoiceDownload::ExtractTarBz2(archive, job->baseDir, topDir, ExtractProgress, &ctx, job->cancel, r->error);
	if (r->status == 0)
		r->dir = job->baseDir + L"\\" + topDir;
}

DWORD WINAPI JobThread(LPVOID arg)
{
	Job *job = static_cast<Job*>(arg);
	if (job->type == Job::CATALOG)
	{
		CatalogResult *cr = new CatalogResult();
		VoiceDownload::FetchCatalog(cr->assets, job->cancel, cr->error);
		if (!PostMessageW(job->hDlg, WM_APP_CATALOG, 0, reinterpret_cast<LPARAM>(cr)))
			delete cr;
	}
	else if (job->type == Job::DOWNLOAD)
	{
		for (unsigned int i = 0; i < job->assets.size() && !*job->cancel; i++)
		{
			const VoiceDownload::Asset &a = job->assets[i];
			ItemResult *r = new ItemResult();
			r->name = Utils::Utf8ToWide(a.name);
			std::wstring archive = job->baseDir + L"\\" + r->name + L".part";
			ProgressCtx ctx;
			ctx.hDlg = job->hDlg;
			wchar_t counter[32];
			swprintf(counter, sizeof(counter)/sizeof(counter[0]), L"(%u/%u) ", i + 1, static_cast<unsigned int>(job->assets.size()));
			ctx.prefix = counter + std::wstring(L"Downloading ") + r->name + L"...";
			ctx.lastPercent = -2;
			ctx.lastTick = 0;
			r->status = VoiceDownload::Download(a.url, archive, DownloadProgress, &ctx, job->cancel, r->error);
			if (r->status == 0)
				InstallArchive(job, archive, r->name, r);
			DeleteFileW(archive.c_str());
			if (!PostMessageW(job->hDlg, WM_APP_ITEM_DONE, 0, reinterpret_cast<LPARAM>(r)))
				delete r;
		}
	}
	else
	{
		ItemResult *r = new ItemResult();
		size_t pos = job->localFile.find_last_of(L"\\/");
		r->name = pos == std::wstring::npos ? job->localFile : job->localFile.substr(pos + 1);
		InstallArchive(job, job->localFile, r->name, r);
		if (!PostMessageW(job->hDlg, WM_APP_ITEM_DONE, 0, reinterpret_cast<LPARAM>(r)))
			delete r;
	}
	PostMessageW(job->hDlg, WM_APP_JOB_DONE, 0, 0);
	delete job;
	return 0;
}

void SetStatus(HWND hDlg, const std::wstring &text)
{
	SetDlgItemTextW(hDlg, IDC_DL_STATUS, text.c_str());
}

void SetProgress(HWND hDlg, int percent)
{
	HWND hProgress = GetDlgItem(hDlg, IDC_DL_PROGRESS);
	SendMessageW(hProgress, PBM_SETPOS, percent < 0 ? 0 : percent, 0);
}

bool IsBusy(DialogState *st)
{
	return st->hThread != NULL;
}

void UpdateButtons(HWND hDlg, DialogState *st)
{
	bool busy = IsBusy(st);
	EnableWindow(GetDlgItem(hDlg, IDC_DL_START), !busy && !st->catalog.empty());
	EnableWindow(GetDlgItem(hDlg, IDC_DL_REFRESH), !busy);
	EnableWindow(GetDlgItem(hDlg, IDC_DL_FROM_FILE), !busy);
	SetDlgItemTextW(hDlg, IDCANCEL, busy ? L"Cancel" : L"Close");
}

bool StartJob(HWND hDlg, DialogState *st, Job *job)
{
	if (IsBusy(st))
	{
		delete job;
		return false;
	}
	if (!Utils::DirectoryExists(st->baseDir) && !CreateDirectoryW(st->baseDir.c_str(), NULL))
	{
		SetStatus(hDlg, L"Cannot create directory " + st->baseDir);
		delete job;
		return false;
	}
	st->cancel = 0;
	job->hDlg = hDlg;
	job->baseDir = st->baseDir;
	job->cancel = &st->cancel;
	DWORD id;
	st->hThread = CreateThread(NULL, 0, JobThread, job, 0, &id);
	if (st->hThread == NULL)
	{
		delete job;
		return false;
	}
	UpdateButtons(hDlg, st);
	return true;
}

std::wstring Lower(std::wstring s)
{
	if (!s.empty())
		CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
	return s;
}

void FillList(HWND hDlg, DialogState *st)
{
	HWND hList = GetDlgItem(hDlg, IDC_DL_LIST);
	SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
	SendMessageW(hList, LB_RESETCONTENT, 0, 0);
	st->listToCatalog.clear();
	std::wstring filter;
	{
		HWND hFilter = GetDlgItem(hDlg, IDC_DL_FILTER);
		int len = GetWindowTextLengthW(hFilter);
		std::vector<wchar_t> buf(len + 1);
		GetWindowTextW(hFilter, &buf[0], len + 1);
		filter = Lower(Utils::Trim(std::wstring(&buf[0])));
	}
	bool int8Only = IsDlgButtonChecked(hDlg, IDC_DL_INT8) == BST_CHECKED;
	for (unsigned int i = 0; i < st->catalog.size(); i++)
	{
		const VoiceDownload::Asset &a = st->catalog[i];
		std::wstring name = Utils::Utf8ToWide(a.name);
		std::wstring lower = Lower(name);
		if (!filter.empty() && lower.find(filter) == std::wstring::npos)
			continue;
		if (int8Only && lower.find(L"int8") == std::wstring::npos)
			continue;
		const std::wstring ext = L".tar.bz2";
		if (name.size() > ext.size())
			name.erase(name.size() - ext.size());
		wchar_t size[32];
		swprintf(size, sizeof(size)/sizeof(size[0]), L"   (%u MB)", static_cast<unsigned int>((a.size + 524288) / 1048576));
		std::wstring item = name + size;
		if (Utils::DirectoryExists(st->baseDir + L"\\" + name))
			item += L"   [installed]";
		SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
		st->listToCatalog.push_back(i);
	}
	SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(hList, NULL, TRUE);
}

void StartCatalog(HWND hDlg, DialogState *st)
{
	Job *job = new Job();
	job->type = Job::CATALOG;
	if (StartJob(hDlg, st, job))
	{
		SetStatus(hDlg, L"Getting list of voices from GitHub...");
		SetProgress(hDlg, 0);
	}
}

void StartDownload(HWND hDlg, DialogState *st)
{
	HWND hList = GetDlgItem(hDlg, IDC_DL_LIST);
	int count = static_cast<int>(SendMessageW(hList, LB_GETSELCOUNT, 0, 0));
	if (count <= 0)
	{
		SetStatus(hDlg, L"Select voice(s) to download (Ctrl / Shift + click: more voices)");
		return;
	}
	std::vector<int> sel(count);
	SendMessageW(hList, LB_GETSELITEMS, count, reinterpret_cast<LPARAM>(&sel[0]));
	Job *job = new Job();
	job->type = Job::DOWNLOAD;
	for (int i = 0; i < count; i++)
	{
		if (sel[i] >= 0 && sel[i] < static_cast<int>(st->listToCatalog.size()))
			job->assets.push_back(st->catalog[st->listToCatalog[sel[i]]]);
	}
	StartJob(hDlg, st, job);
}

void StartFromFile(HWND hDlg, DialogState *st)
{
	wchar_t file[MAX_PATH * 2] = L"";
	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hDlg;
	ofn.lpstrFilter = L"Voice archive (*.tar.bz2)\0*.tar.bz2\0All files\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = sizeof(file)/sizeof(file[0]);
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&ofn))
		return;
	Job *job = new Job();
	job->type = Job::LOCAL_FILE;
	job->localFile = file;
	StartJob(hDlg, st, job);
}

std::wstring DefaultFilter(void)
{
	// user language, e.g. pl-PL -> pl_PL
	wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
	if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) <= 0)
		return std::wstring();
	std::wstring s = locale;
	if (s.size() != 5)
		return std::wstring();
	s[2] = L'_';
	return s;
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	DialogState *st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hDlg, DWLP_USER));
	switch (msg)
	{
	case WM_INITDIALOG:
		st = reinterpret_cast<DialogState*>(lParam);
		SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
		st->hDlg = hDlg;
		SetDlgItemTextW(hDlg, IDC_DL_FILTER, DefaultFilter().c_str());
		SendDlgItemMessageW(hDlg, IDC_DL_PROGRESS, PBM_SETRANGE32, 0, 100);
		UpdateButtons(hDlg, st);
		StartCatalog(hDlg, st);
		return TRUE;
	case WM_APP_CATALOG:
	{
		CatalogResult *cr = reinterpret_cast<CatalogResult*>(lParam);
		if (cr->error.empty())
		{
			st->catalog.swap(cr->assets);
			wchar_t buf[96];
			swprintf(buf, sizeof(buf)/sizeof(buf[0]), L"%u voices available", static_cast<unsigned int>(st->catalog.size()));
			SetStatus(hDlg, buf);
		}
		else
		{
			SetStatus(hDlg, L"Failed to get list of voices: " + Utils::Utf8ToWide(cr->error));
		}
		delete cr;
		FillList(hDlg, st);
		return TRUE;
	}
	case WM_APP_PROGRESS:
	{
		std::wstring *s = reinterpret_cast<std::wstring*>(lParam);
		SetStatus(hDlg, *s);
		SetProgress(hDlg, static_cast<int>(wParam));
		delete s;
		return TRUE;
	}
	case WM_APP_ITEM_DONE:
	{
		ItemResult *r = reinterpret_cast<ItemResult*>(lParam);
		if (r->status == 0)
		{
			st->installedDirs->push_back(r->dir);
			if (!st->installedNames.empty())
				st->installedNames += L", ";
			st->installedNames += r->dir.substr(r->dir.find_last_of(L'\\') + 1);
			LOG("Voice installed: %s\n", Utils::WideToUtf8(r->dir).c_str());
		}
		else if (r->status != 2)
		{
			std::wstring text = r->name + L": " + Utils::Utf8ToWide(r->error);
			LOG("Voice installation failed: %s\n", Utils::WideToUtf8(text).c_str());
			MessageBoxW(hDlg, text.c_str(), L"Download voices", MB_ICONERROR);
		}
		delete r;
		return TRUE;
	}
	case WM_APP_JOB_DONE:
		if (st->hThread)
		{
			WaitForSingleObject(st->hThread, INFINITE);
			CloseHandle(st->hThread);
			st->hThread = NULL;
		}
		if (st->closeRequested)
		{
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		}
		SetProgress(hDlg, 0);
		if (st->cancel)
			SetStatus(hDlg, L"Cancelled");
		else if (!st->installedNames.empty())
			SetStatus(hDlg, L"Installed: " + st->installedNames + L" (added to voice list)");
		if (!st->catalog.empty())
			FillList(hDlg, st);
		UpdateButtons(hDlg, st);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_DL_FILTER:
			if (HIWORD(wParam) == EN_CHANGE)
				FillList(hDlg, st);
			return TRUE;
		case IDC_DL_INT8:
			FillList(hDlg, st);
			return TRUE;
		case IDC_DL_REFRESH:
			StartCatalog(hDlg, st);
			return TRUE;
		case IDC_DL_START:
			StartDownload(hDlg, st);
			return TRUE;
		case IDC_DL_LIST:
			if (HIWORD(wParam) == LBN_DBLCLK)
				StartDownload(hDlg, st);
			return TRUE;
		case IDC_DL_FROM_FILE:
			StartFromFile(hDlg, st);
			return TRUE;
		case IDCANCEL:
			if (IsBusy(st))
			{
				// wait for thread to finish (cancelled), dialog is closed on WM_APP_JOB_DONE
				// first press: cancel job; second press (or Esc / close button again): close dialog when job ends
				if (st->cancel)
					st->closeRequested = true;
				InterlockedExchange(&st->cancel, 1);
				SetStatus(hDlg, L"Cancelling...");
				return TRUE;
			}
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

}	// namespace

void DownloadDialog::Show(HINSTANCE hInstance, HWND parent, const std::wstring &baseDir, std::vector<std::wstring> &installedDirs)
{
	DialogState st;
	st.baseDir = baseDir;
	st.installedDirs = &installedDirs;
	st.hThread = NULL;
	st.cancel = 0;
	st.closeRequested = false;
	st.hDlg = NULL;
	DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_DOWNLOAD), parent, DlgProc, reinterpret_cast<LPARAM>(&st));
	if (st.hThread)
	{
		// dialog closed by other means while thread is running
		InterlockedExchange(&st.cancel, 1);
		WaitForSingleObject(st.hThread, INFINITE);
		CloseHandle(st.hThread);
		MSG m;
		// only messages for this dialog (other windows of the thread may use the same WM_APP range)
		while (st.hDlg && PeekMessageW(&m, st.hDlg, WM_APP_CATALOG, WM_APP_JOB_DONE, PM_REMOVE))
		{
			if (m.message == WM_APP_CATALOG)
				delete reinterpret_cast<CatalogResult*>(m.lParam);
			else if (m.message == WM_APP_PROGRESS)
				delete reinterpret_cast<std::wstring*>(m.lParam);
			else if (m.message == WM_APP_ITEM_DONE)
				delete reinterpret_cast<ItemResult*>(m.lParam);
		}
	}
}
