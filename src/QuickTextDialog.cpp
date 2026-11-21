#include "QuickTextDialog.h"
#include "Utils.h"
#include "resource.h"
#include <vector>

namespace
{

struct DialogState
{
	QuickText *qt;
	const std::vector<VoiceConfig> *voices;
};

std::wstring GetText(HWND hDlg, int id)
{
	HWND h = GetDlgItem(hDlg, id);
	int len = GetWindowTextLengthW(h);
	std::vector<wchar_t> buf(len + 1);
	GetWindowTextW(h, &buf[0], len + 1);
	return std::wstring(&buf[0]);
}

/** CRLF for multi-line edit control */
std::wstring ToCrLf(const std::wstring &s)
{
	std::wstring out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r'))
			out += L'\r';
		out += s[i];
	}
	return out;
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	DialogState *st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hDlg, DWLP_USER));
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		st = reinterpret_cast<DialogState*>(lParam);
		SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
		SetDlgItemTextW(hDlg, IDC_QT_CAPTION, Utils::Utf8ToWide(st->qt->caption).c_str());
		SetDlgItemTextW(hDlg, IDC_QT_TEXT, ToCrLf(Utils::Utf8ToWide(st->qt->text)).c_str());
		HWND hVoice = GetDlgItem(hDlg, IDC_QT_VOICE);
		SendMessageW(hVoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(current voice)"));
		int sel = 0;
		for (unsigned int i = 0; i < st->voices->size(); i++)
		{
			const std::string &name = (*st->voices)[i].name;
			SendMessageW(hVoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utils::Utf8ToWide(name).c_str()));
			if (!st->qt->voice.empty() && name == st->qt->voice)
				sel = static_cast<int>(i) + 1;
		}
		if (!st->qt->voice.empty() && sel == 0)
		{
			// voice removed from settings: keep it visible
			SendMessageW(hVoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((Utils::Utf8ToWide(st->qt->voice) + L" (missing)").c_str()));
			sel = static_cast<int>(st->voices->size()) + 1;
		}
		SendMessageW(hVoice, CB_SETCURSEL, sel, 0);
		SetFocus(GetDlgItem(hDlg, st->qt->text.empty() ? IDC_QT_TEXT : IDC_QT_CAPTION));
		return FALSE;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
		{
			st->qt->caption = Utils::WideToUtf8(Utils::Trim(GetText(hDlg, IDC_QT_CAPTION)));
			std::wstring text = Utils::Trim(GetText(hDlg, IDC_QT_TEXT));
			std::wstring lf;
			for (size_t i = 0; i < text.size(); i++)
			{
				if (text[i] != L'\r')
					lf += text[i];
			}
			st->qt->text = Utils::WideToUtf8(lf);
			int sel = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_QT_VOICE, CB_GETCURSEL, 0, 0));
			if (sel <= 0)
				st->qt->voice.clear();
			else if (sel <= static_cast<int>(st->voices->size()))
				st->qt->voice = (*st->voices)[sel - 1].name;
			// else: missing voice kept unchanged
			EndDialog(hDlg, IDOK);
			return TRUE;
		}
		case IDCANCEL:
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

bool QuickTextDialog::Show(HINSTANCE hInstance, HWND parent, QuickText &qt, const std::vector<VoiceConfig> &voices)
{
	QuickText edited = qt;
	DialogState st;
	st.qt = &edited;
	st.voices = &voices;
	if (DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_QUICK_TEXT), parent, DlgProc, reinterpret_cast<LPARAM>(&st)) != IDOK)
		return false;
	qt = edited;
	return true;
}
