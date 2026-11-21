#include "Plugin.h"
#include "Host.h"
#include "Log.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "QuickTextDialog.h"
#include "T2S.h"
#include "TtsWorker.h"
#include "Utils.h"
#include "Voices.h"
#include "resource.h"
#include "tSIP_plugin_v2.h"

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

enum
{
	WM_APP_WORKER_RESULT = WM_APP + 1,
	WM_APP_HOST_EVENT,			///< lParam = std::string* "type\njson"
};

enum
{
	TIMER_PLAYBACK = 1,			///< checking end of playback
	TIMER_EMBED = 2				///< embedded window: following button size, re-embedding
};

enum
{
	PLAYBACK_POLL_MS = 150,
	EMBED_CHECK_MS = 500,
	MAX_HISTORY_CHARS = 64 * 1024
};

/** Silence added around speech: generated speech starts almost immediately (~40 ms) and
	beginning of the first word was cut off while audio output / call audio source was starting */
enum
{
	LEAD_SILENCE_CALL_MS = 150,
	LEAD_SILENCE_LOCAL_MS = 300,
	TAIL_SILENCE_MS = 100
};

enum
{
	IDC_VOICE = 100,
	IDC_STATUS,
	IDC_MENU,
	IDC_HISTORY,
	IDC_PLAYING,
	IDC_REPEAT,
	IDC_STOP,
	IDC_INPUT,
	IDC_SEND,
	IDC_ENTER_SENDS,
	IDC_SIDETONE,
	IDC_SIDETONE_LEVEL,
	IDC_CLEAR,
	IDC_QUICK_FIRST = 130	///< quick text buttons: IDC_QUICK_FIRST ... + QUICK_TEXT_COUNT - 1
};

enum
{
	IDM_SETTINGS = 200,
	IDM_CLEAR_HISTORY,
	IDM_ALWAYS_ON_TOP,
	IDM_SHOW_VOICE,
	IDM_SHOW_QUICK,
	IDM_QT_SEND,
	IDM_QT_INSERT,
	IDM_QT_EDIT,
	IDM_QT_CLEAR,
	IDM_QT_CLICK_SENDS,
	IDM_ABOUT
};

const wchar_t* const MSG_WND_CLASS = L"tSIP_TTS_sherpa_onnx_msg";
const wchar_t* const UI_WND_CLASS = L"tSIP_TTS_sherpa_onnx_ui";

/** Message ready to be played */
struct Item
{
	std::wstring text;
	std::wstring voiceName;
	std::vector<short> pcm;
	int sampleRate;
	Item(void): sampleRate(0) {}
};

struct PendingSpeech
{
	std::wstring text;
	std::wstring voiceName;
};

enum PlayMode
{
	PLAY_NONE = 0,
	PLAY_CALL,		///< audio input of the call switched to WAV file
	PLAY_LOCAL		///< no call: played locally (preview)
};

bool started = false;
HINSTANCE hInstance = NULL;
std::wstring baseDir;			///< plugin data directory: libraries, models (relative paths)
std::wstring configPath;
std::wstring profileDir;		///< tSIP profile directory - WAV files for PlayAudioFile
std::wstring dllName;

HWND hMsgWnd = NULL;			///< message-only window: worker results, events, timers
HWND hUiWnd = NULL;				///< plugin window (top-level or child of tSIP button)
HWND hEmbedParent = NULL;		///< button window if embedded
bool embedFailedLogged = false;
bool destroyingUi = false;
HFONT hFont = NULL;
WNDPROC origInputProc = NULL;

// controls
HWND hVoice, hStatus, hMenuBtn, hHistory, hPlaying, hClear, hRepeat, hStop, hInput, hSend, hEnterSends, hSidetone, hSidetoneLevel;
HWND hQuick[Settings::QUICK_TEXT_COUNT];
HWND hTooltip;

std::wstring historyText;
std::wstring statusText;

unsigned int nextRequestId = 1;
std::map<unsigned int, PendingSpeech> pendingSpeech;
std::deque<Item> playQueue;
Item current;
PlayMode playMode = PLAY_NONE;
unsigned int playCallUid = 0;
int playBaseErrorCount = 0;
DWORD playStartTick = 0;
DWORD playDurationMs = 0;
std::vector<char> localWav;		///< buffer for PlaySound (SND_MEMORY) - must stay valid while playing
int wavIndex = 0;
Item lastItem;
bool haveLastItem = false;

std::set<unsigned int> touchedCalls;	///< calls with audio input changed by plugin - restored on stop
std::set<unsigned int> establishedCalls;	///< calls in ESTABLISHED state (from call_state events)
std::set<unsigned int> sidetoneCalls;		///< calls with sidetone enabled by plugin - disabled on stop
bool workerRunning = false;

std::wstring SilenceFileName(void)
{
	return dllName + L"_silence.wav";
}

std::wstring Now(void)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t buf[16];
	swprintf(buf, sizeof(buf)/sizeof(buf[0]), L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
	return buf;
}

void AddHistory(const std::wstring &line)
{
	std::wstring text = L"[" + Now() + L"] " + line;
	// multi-line edit control: CRLF line endings
	std::wstring converted;
	for (size_t i = 0; i < text.size(); i++)
	{
		if (text[i] == L'\n' && (i == 0 || text[i-1] != L'\r'))
			converted += L'\r';
		converted += text[i];
	}
	converted += L"\r\n";
	historyText += converted;
	if (historyText.size() > MAX_HISTORY_CHARS)
	{
		size_t cut = historyText.find(L"\r\n", historyText.size() - MAX_HISTORY_CHARS);
		historyText.erase(0, cut == std::wstring::npos ? historyText.size() - MAX_HISTORY_CHARS : cut + 2);
		if (hHistory)
			SetWindowTextW(hHistory, historyText.c_str());
	}
	else if (hHistory)
	{
		int len = GetWindowTextLengthW(hHistory);
		SendMessageW(hHistory, EM_SETSEL, len, len);
		SendMessageW(hHistory, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(converted.c_str()));
	}
	if (hHistory)
		SendMessageW(hHistory, EM_SCROLLCARET, 0, 0);
}

void AddHistoryError(const std::string &errorUtf8)
{
	AddHistory(L"! " + Utils::Utf8ToWide(errorUtf8));
}

void SetStatus(const std::wstring &text)
{
	statusText = text;
	if (hStatus)
		SetWindowTextW(hStatus, text.c_str());
}

std::wstring ResolveUtf8Path(const std::string &path)
{
	return Utils::ResolvePath(baseDir, Utils::Utf8ToWide(path));
}

T2S::EngineConfig MakeEngineConfig(const VoiceConfig &v)
{
	T2S::EngineConfig cfg;
	cfg.libraryDir = appSettings.libraryDir.empty() ? baseDir : ResolveUtf8Path(appSettings.libraryDir);
	cfg.numThreads = appSettings.numThreads;
	cfg.modelType = v.modelType;
	cfg.model = ResolveUtf8Path(v.model);
	cfg.tokens = ResolveUtf8Path(v.tokens);
	cfg.dataDir = ResolveUtf8Path(v.dataDir);
	cfg.lexicon = ResolveUtf8Path(v.lexicon);
	cfg.voices = ResolveUtf8Path(v.voices);
	cfg.lang = v.lang;
	return cfg;
}

void PreloadCurrentVoice(void)
{
	const VoiceConfig *v = appSettings.GetCurrentVoice();
	if (v == NULL)
	{
		SetStatus(L"No voice - Settings / Download voices");
		return;
	}
	TtsWorker::Request req;
	req.type = TtsWorker::Request::LOAD;
	req.id = nextRequestId++;
	req.engine = MakeEngineConfig(*v);
	TtsWorker::Post(req);
	SetStatus(L"Loading...");
}

bool IsBusy(void)
{
	return playMode != PLAY_NONE || !pendingSpeech.empty() || !playQueue.empty();
}

void UpdatePlayingUi(void)
{
	if (hUiWnd == NULL)
		return;
	std::wstring text;
	if (playMode != PLAY_NONE)
	{
		text = (playMode == PLAY_LOCAL ? L"Playing locally (no call): " : L"Playing: ") + current.text;
	}
	else if (!pendingSpeech.empty())
	{
		text = L"Generating...";
	}
	size_t waiting = playQueue.size() + (playMode != PLAY_NONE ? pendingSpeech.size() : 0);
	if (waiting > 0)
	{
		wchar_t buf[48];
		swprintf(buf, sizeof(buf)/sizeof(buf[0]), L" (+%u waiting)", static_cast<unsigned int>(waiting));
		text += buf;
	}
	for (size_t i = 0; i < text.size(); i++)
	{
		if (text[i] == L'\r' || text[i] == L'\n')
			text[i] = L' ';
	}
	SetWindowTextW(hPlaying, text.c_str());
	EnableWindow(hStop, IsBusy());
	EnableWindow(hRepeat, haveLastItem);
}

/** Play WAV file into call; uid = 0: current call
	\return TSIP_OK, TSIP_ERR_NOT_AVAILABLE if there is no call (error is empty) or other error
*/
int PlayFileInCall(unsigned int uid, const std::wstring &fileName, unsigned int &resultUid, int &errorCount, std::string &error)
{
	Json::Value params(Json::objectValue), result;
	params["file"] = Utils::WideToUtf8(fileName);
	if (uid)
		params["uid"] = uid;
	int status = HostInvoke("PlayAudioFile", params, result);
	if (result["error"].isString())
		error = result["error"].asString();
	if (status == TSIP_OK)
	{
		resultUid = result.get("uid", 0).asUInt();
		errorCount = result.get("audioErrorCount", 0).asInt();
		touchedCalls.insert(resultUid);
	}
	else if (error.empty() && status != TSIP_ERR_NOT_AVAILABLE)
	{
		error = std::string("PlayAudioFile: ") + HostErrorName(status);
	}
	return status;
}

/** Restore configured audio input (microphone) of the call */
void RestoreMicrophone(unsigned int uid)
{
	Json::Value params(Json::objectValue), result;
	params["uid"] = uid;
	params["configured"] = true;
	HostInvoke("SetAudioInput", params, result);
}

/** Silence or microphone after message, depending on settings */
void SetIdleAudioInput(unsigned int uid)
{
	if (appSettings.silenceMicrophone)
	{
		unsigned int resultUid;
		int errorCount;
		std::string error;
		PlayFileInCall(uid, SilenceFileName(), resultUid, errorCount, error);
	}
	else
	{
		RestoreMicrophone(uid);
	}
}

void SetSidetone(unsigned int uid, bool state)
{
	Json::Value params(Json::objectValue), result;
	params["uid"] = uid;
	params["state"] = state;
	params["level"] = appSettings.sidetoneLevel;
	int status = HostInvoke("Sidetone", params, result);
	if (status == TSIP_OK && state)
		sidetoneCalls.insert(uid);
	else
		sidetoneCalls.erase(uid);
	if (status != TSIP_OK && status != TSIP_ERR_NOT_AVAILABLE)
		LOG("Sidetone: %s\n", HostErrorName(status));
}

/** Apply sidetone setting (state, level) to all established calls, switch off where enabled before */
void ApplySidetone(void)
{
	std::set<unsigned int> uids = establishedCalls;
	uids.insert(sidetoneCalls.begin(), sidetoneCalls.end());
	for (std::set<unsigned int>::const_iterator it = uids.begin(); it != uids.end(); ++it)
	{
		if (appSettings.sidetone || sidetoneCalls.count(*it))
			SetSidetone(*it, appSettings.sidetone);
	}
}

/** Call to play message into: current call if it is established, otherwise any established call;
	0 if there is no established call (message is played locally) */
unsigned int ChooseTargetCall(void)
{
	if (establishedCalls.empty())
		return 0;
	Json::Value result;
	if (HostInvoke("GetCallAudioState", Json::Value(Json::objectValue), result) == TSIP_OK)
	{
		unsigned int uid = result.get("uid", 0).asUInt();
		if (establishedCalls.count(uid))
			return uid;
	}
	return *establishedCalls.begin();
}

void PlayNext(void);

/** WAV image of the item with silence before and after speech */
void MakePaddedWav(const Item &item, unsigned int leadMs, std::vector<char> &wav, DWORD &durationMs)
{
	std::vector<short> pcm;
	size_t lead = static_cast<size_t>(item.sampleRate) * leadMs / 1000;
	size_t tail = static_cast<size_t>(item.sampleRate) * TAIL_SILENCE_MS / 1000;
	pcm.reserve(lead + item.pcm.size() + tail);
	pcm.resize(lead, 0);
	pcm.insert(pcm.end(), item.pcm.begin(), item.pcm.end());
	pcm.resize(pcm.size() + tail, 0);
	T2S::MakeWav(pcm, item.sampleRate, wav);
	durationMs = item.sampleRate > 0 ? static_cast<DWORD>(pcm.size() * 1000ULL / item.sampleRate) : 0;
}

void FinishPlayback(const wchar_t *note)
{
	if (playMode == PLAY_NONE)
		return;
	if (hMsgWnd)
		KillTimer(hMsgWnd, TIMER_PLAYBACK);
	if (playMode == PLAY_LOCAL)
	{
		PlaySoundW(NULL, NULL, 0);
		localWav.clear();
	}
	std::wstring line = appSettings.showVoiceInHistory ? (L"(" + current.voiceName + L") " + current.text) : current.text;
	if (note)
		line += note;
	AddHistory(line);
	PlayMode prevMode = playMode;
	unsigned int prevUid = playCallUid;
	playMode = PLAY_NONE;
	current = Item();

	if (!playQueue.empty())
		PlayNext();
	// next message is not played into the same call (other call, error): give microphone back
	if (prevMode == PLAY_CALL && !appSettings.silenceMicrophone && note == NULL &&
		!(playMode == PLAY_CALL && playCallUid == prevUid))
	{
		RestoreMicrophone(prevUid);
	}
	UpdatePlayingUi();
}

void PlayNext(void)
{
	while (playMode == PLAY_NONE && !playQueue.empty())
	{
		current = playQueue.front();
		playQueue.pop_front();
		lastItem = current;
		haveLastItem = true;

		std::vector<char> wav;
		MakePaddedWav(current, LEAD_SILENCE_CALL_MS, wav, playDurationMs);

		// alternate files: new file is not the one being read by audio source of the call
		wavIndex = (wavIndex + 1) % 2;
		wchar_t suffix[16];
		swprintf(suffix, sizeof(suffix)/sizeof(suffix[0]), L"_%d.wav", wavIndex);
		std::wstring fileName = dllName + suffix;
		if (profileDir.empty() || T2S::WriteFile(profileDir + L"\\" + fileName, wav) != 0)
		{
			AddHistoryError("failed to write WAV file to tSIP profile directory: " + Utils::WideToUtf8(profileDir));
			continue;
		}

		std::string error;
		unsigned int uid = 0;
		int errorCount = 0;
		unsigned int target = ChooseTargetCall();
		int status = target ? PlayFileInCall(target, fileName, uid, errorCount, error) : TSIP_ERR_NOT_AVAILABLE;
		if (status == TSIP_OK)
		{
			playMode = PLAY_CALL;
			playCallUid = uid;
			playBaseErrorCount = errorCount;
		}
		else if (status == TSIP_ERR_NOT_AVAILABLE && error.empty())
		{
			// no call: preview
			MakePaddedWav(current, LEAD_SILENCE_LOCAL_MS, localWav, playDurationMs);
			if (!PlaySoundW(reinterpret_cast<LPCWSTR>(&localWav[0]), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
			{
				localWav.clear();
				AddHistoryError("failed to play audio locally");
				continue;
			}
			playMode = PLAY_LOCAL;
		}
		else
		{
			AddHistoryError(error);
			continue;
		}
		playStartTick = GetTickCount();
		SetTimer(hMsgWnd, TIMER_PLAYBACK, PLAYBACK_POLL_MS, NULL);
	}
	UpdatePlayingUi();
}

void CheckPlayback(void)
{
	if (playMode == PLAY_NONE)
	{
		KillTimer(hMsgWnd, TIMER_PLAYBACK);
		return;
	}
	DWORD elapsed = GetTickCount() - playStartTick;
	if (playMode == PLAY_LOCAL)
	{
		if (elapsed >= playDurationMs + 200)
			FinishPlayback(NULL);
		return;
	}
	Json::Value params(Json::objectValue), result;
	params["uid"] = playCallUid;
	int status = HostInvoke("GetCallAudioState", params, result);
	if (status == TSIP_ERR_NOT_AVAILABLE)
	{
		playQueue.clear();
		FinishPlayback(L" [call ended]");
		return;
	}
	if (status == TSIP_OK && elapsed < 300)
	{
		// end of previous file (e.g. silence) may be reported shortly after switching to this one
		playBaseErrorCount = result.get("audioErrorCount", 0).asInt();
		return;
	}
	if (status == TSIP_OK && result.get("audioErrorCount", 0).asInt() != playBaseErrorCount)
	{
		FinishPlayback(NULL);	// end of file
		return;
	}
	if (elapsed > playDurationMs + 5000)
	{
		LOG("End of playback not reported, assuming finished\n");
		FinishPlayback(NULL);
	}
}

void StopAll(void)
{
	TtsWorker::CancelSpeech();
	pendingSpeech.clear();
	playQueue.clear();
	if (playMode != PLAY_NONE)
	{
		PlayMode prevMode = playMode;
		unsigned int prevUid = playCallUid;
		FinishPlayback(L" [stopped]");
		if (prevMode == PLAY_CALL)
			SetIdleAudioInput(prevUid);
	}
	UpdatePlayingUi();
}

/** \param voiceIndex voice to use, -1: current voice */
void Speak(const std::wstring &text, int voiceIndex = -1)
{
	const VoiceConfig *v = (voiceIndex >= 0 && voiceIndex < static_cast<int>(appSettings.voices.size())) ?
		&appSettings.voices[voiceIndex] : appSettings.GetCurrentVoice();
	if (v == NULL)
	{
		AddHistoryError("no voice configured - see Settings");
		return;
	}
	TtsWorker::Request req;
	req.type = TtsWorker::Request::SPEAK;
	req.id = nextRequestId++;
	req.engine = MakeEngineConfig(*v);
	req.textUtf8 = Utils::WideToUtf8(text);
	req.speakerId = v->speakerId;
	req.speed = static_cast<float>(v->speed);
	PendingSpeech ps;
	ps.text = text;
	ps.voiceName = Utils::Utf8ToWide(v->name);
	if (!workerRunning)
	{
		AddHistoryError("speech engine thread not running");
		return;
	}
	pendingSpeech[req.id] = ps;
	TtsWorker::Post(req);
	UpdatePlayingUi();
}

void SendInput(void)
{
	if (hInput == NULL)
		return;
	int len = GetWindowTextLengthW(hInput);
	std::vector<wchar_t> buf(len + 1);
	GetWindowTextW(hInput, &buf[0], len + 1);
	std::wstring text = Utils::Trim(std::wstring(&buf[0]));
	if (text.empty())
		return;
	SetWindowTextW(hInput, L"");
	Speak(text);
}

void Repeat(void)
{
	if (!haveLastItem)
		return;
	playQueue.push_back(lastItem);
	if (playMode == PLAY_NONE)
		PlayNext();
	UpdatePlayingUi();
}

void OnWorkerResult(TtsWorker::Result *r)
{
	switch (r->type)
	{
	case TtsWorker::Request::LOAD:
		if (r->status == 0)
		{
			SetStatus(L"");	// voice name is shown in combo box
		}
		else
		{
			SetStatus(L"Error: " + Utils::Utf8ToWide(r->error));
			AddHistoryError(r->error);
		}
		break;
	case TtsWorker::Request::SPEAK:
	{
		std::map<unsigned int, PendingSpeech>::iterator it = pendingSpeech.find(r->id);
		if (it == pendingSpeech.end())
			break;	// stopped by user
		PendingSpeech ps = it->second;
		pendingSpeech.erase(it);
		if (r->status == 0)
		{
			Item item;
			item.text = ps.text;
			item.voiceName = ps.voiceName;
			item.pcm.swap(r->pcm);
			item.sampleRate = r->sampleRate;
			playQueue.push_back(item);
			if (playMode == PLAY_NONE)
				PlayNext();
			if (statusText.compare(0, 6, L"Error:") == 0)
				SetStatus(L"");
		}
		else if (r->status != 1)
		{
			AddHistory((appSettings.showVoiceInHistory ? L"(" + ps.voiceName + L") " : std::wstring()) + ps.text + L" [not spoken]");
			AddHistoryError(r->error);
			SetStatus(L"Error: " + Utils::Utf8ToWide(r->error));
		}
		break;
	}
	default:
		break;
	}
	UpdatePlayingUi();
}

void OnCallStateEvent(const Json::Value &jv)
{
	unsigned int uid = jv.get("uid", 0).asUInt();
	std::string state = jv.get("stateName", "").asString();
	if (uid == 0)
		return;
	if (state == "ESTABLISHED")
	{
		establishedCalls.insert(uid);
		bool playingInThisCall = (playMode == PLAY_CALL && playCallUid == uid);
		if (appSettings.silenceMicrophone && !playingInThisCall)
		{
			unsigned int resultUid;
			int errorCount;
			std::string error;
			if (PlayFileInCall(uid, SilenceFileName(), resultUid, errorCount, error) != TSIP_OK)
				LOG("Failed to replace microphone with silence: %s\n", error.c_str());
		}
		if (appSettings.sidetone)
			SetSidetone(uid, true);
	}
	else if (state == "CLOSED")
	{
		touchedCalls.erase(uid);
		establishedCalls.erase(uid);
		sidetoneCalls.erase(uid);
		if (playMode == PLAY_CALL && playCallUid == uid)
		{
			playQueue.clear();
			FinishPlayback(L" [call ended]");
		}
	}
}

void ApplyAlwaysOnTop(void)
{
	if (hUiWnd && hEmbedParent == NULL)
	{
		SetWindowPos(hUiWnd, appSettings.wnd.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void FillVoiceCombo(void)
{
	if (hVoice == NULL)
		return;
	SendMessageW(hVoice, CB_RESETCONTENT, 0, 0);
	for (unsigned int i = 0; i < appSettings.voices.size(); i++)
	{
		std::wstring name = Utils::Utf8ToWide(appSettings.voices[i].name);
		if (name.empty())
			name = L"(unnamed)";
		if (!Voices::IsInstalled(baseDir, appSettings.voices[i]))
			name += L" (missing)";
		SendMessageW(hVoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
	}
	SendMessageW(hVoice, CB_SETCURSEL, appSettings.currentVoice, 0);
}

void LayoutUi(void);

std::wstring QuickCaption(unsigned int i)
{
	const QuickText &qt = appSettings.quickTexts[i];
	std::wstring caption = Utils::Utf8ToWide(qt.caption);
	if (caption.empty())
	{
		caption = Utils::Utf8ToWide(qt.text);
		for (size_t j = 0; j < caption.size(); j++)
		{
			if (caption[j] == L'\n' || caption[j] == L'\r' || caption[j] == L'\t')
				caption[j] = L' ';
		}
		if (caption.size() > 11)
			caption = caption.substr(0, 9) + L"...";
	}
	if (caption.empty())
		caption = L"+";
	return caption;
}

void UpdateQuickButton(unsigned int i)
{
	if (hQuick[i] == NULL)
		return;
	SetWindowTextW(hQuick[i], QuickCaption(i).c_str());
	if (hTooltip)
	{
		const QuickText &qt = appSettings.quickTexts[i];
		wchar_t key[32];
		swprintf(key, sizeof(key)/sizeof(key[0]), L"\r\n[Ctrl+%u]", i + 1);
		std::wstring tip = qt.text.empty() ? std::wstring(L"Empty - click to define text") :
			Utils::Utf8ToWide(qt.text) + (qt.voice.empty() ? std::wstring() : L"\r\n(" + Utils::Utf8ToWide(qt.voice) + L")") + key;
		TOOLINFOW ti;
		memset(&ti, 0, sizeof(ti));
		ti.cbSize = TTTOOLINFOW_V2_SIZE;	// accepted by comctl32 5.x and 6.x
		ti.hwnd = hUiWnd;
		ti.uId = reinterpret_cast<UINT_PTR>(hQuick[i]);
		ti.lpszText = const_cast<wchar_t*>(tip.c_str());
		SendMessageW(hTooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
	}
}

void EditQuickText(unsigned int i)
{
	if (QuickTextDialog::Show(hInstance, hUiWnd, appSettings.quickTexts[i], appSettings.voices))
	{
		UpdateQuickButton(i);
		appSettings.Write(configPath);
	}
}

void SpeakQuickText(unsigned int i)
{
	const QuickText &qt = appSettings.quickTexts[i];
	int voiceIndex = -1;
	if (!qt.voice.empty())
	{
		for (unsigned int j = 0; j < appSettings.voices.size(); j++)
		{
			if (appSettings.voices[j].name == qt.voice)
				voiceIndex = static_cast<int>(j);
		}
		if (voiceIndex < 0)
		{
			AddHistoryError("voice \"" + qt.voice + "\" of quick text button not found");
			return;
		}
	}
	Speak(Utils::Utf8ToWide(qt.text), voiceIndex);
}

void InsertQuickText(unsigned int i)
{
	if (hInput == NULL)
		return;
	std::wstring text = Utils::Utf8ToWide(appSettings.quickTexts[i].text);
	std::wstring crlf;
	for (size_t j = 0; j < text.size(); j++)
	{
		if (text[j] == L'\n')
			crlf += L'\r';
		crlf += text[j];
	}
	SendMessageW(hInput, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(crlf.c_str()));
	SetFocus(hInput);
}

void OnQuickClick(unsigned int i)
{
	if (i >= appSettings.quickTexts.size())
		return;
	if (appSettings.quickTexts[i].text.empty())
		EditQuickText(i);
	else if (appSettings.quickTextClickSends)
		SpeakQuickText(i);
	else
		InsertQuickText(i);
}

void ShowQuickMenu(unsigned int i, int x, int y)
{
	const QuickText &qt = appSettings.quickTexts[i];
	UINT textFlags = qt.text.empty() ? MF_GRAYED : 0;
	HMENU hMenu = CreatePopupMenu();
	AppendMenuW(hMenu, MF_STRING | textFlags, IDM_QT_SEND, L"Send");
	AppendMenuW(hMenu, MF_STRING | textFlags, IDM_QT_INSERT, L"Insert into input field");
	AppendMenuW(hMenu, MF_STRING, IDM_QT_EDIT, L"Edit...");
	AppendMenuW(hMenu, MF_STRING | textFlags, IDM_QT_CLEAR, L"Clear");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(hMenu, MF_STRING | (appSettings.quickTextClickSends ? MF_CHECKED : 0), IDM_QT_CLICK_SENDS, L"Click sends immediately");
	AppendMenuW(hMenu, MF_STRING | (appSettings.showQuickTexts ? MF_CHECKED : 0), IDM_SHOW_QUICK, L"Show quick text buttons");
	if (x == -1 && y == -1)
	{
		// keyboard (Shift+F10, menu key): below the button
		RECT rc;
		GetWindowRect(hQuick[i], &rc);
		x = rc.left;
		y = rc.bottom;
	}
	int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, x, y, 0, hUiWnd, NULL);
	DestroyMenu(hMenu);
	switch (cmd)
	{
	case IDM_QT_SEND:
		SpeakQuickText(i);
		break;
	case IDM_QT_INSERT:
		InsertQuickText(i);
		break;
	case IDM_QT_EDIT:
		EditQuickText(i);
		break;
	case IDM_QT_CLEAR:
		appSettings.quickTexts[i] = QuickText();
		UpdateQuickButton(i);
		appSettings.Write(configPath);
		break;
	case IDM_QT_CLICK_SENDS:
		appSettings.quickTextClickSends = !appSettings.quickTextClickSends;
		break;
	case IDM_SHOW_QUICK:
		appSettings.showQuickTexts = !appSettings.showQuickTexts;
		LayoutUi();
		break;
	default:
		break;
	}
}

void LayoutUi(void)
{
	if (hUiWnd == NULL || hInput == NULL)
		return;
	RECT rc;
	GetClientRect(hUiWnd, &rc);
	int w = rc.right, h = rc.bottom;
	const int m = 4;		// margin
	const int rowH = 24;
	const int btnW = 60;
	const int inputH = 52;

	int y = m;
	int x = m;
	int voiceW = w * 3 / 5;
	if (voiceW < 160) voiceW = 160;
	if (voiceW > 320) voiceW = 320;
	MoveWindow(hVoice, x, y, voiceW, 300, TRUE);
	MoveWindow(hMenuBtn, w - m - 28, y, 28, rowH - 2, TRUE);
	MoveWindow(hStatus, x + voiceW + m, y + 4, w - (x + voiceW + m) - 28 - 2 * m, rowH - 6, TRUE);
	y += rowH + 2;

	int bottomRowY = h - m - rowH;
	int inputY = bottomRowY - m - inputH;
	int playingY = inputY - m - rowH;
	if (appSettings.showQuickTexts)
	{
		int quickY = playingY;
		playingY = quickY - m - rowH;
		const int n = Settings::QUICK_TEXT_COUNT;
		int qw = (w - 2 * m - (n - 1) * m) / n;
		if (qw < 10) qw = 10;
		for (int i = 0; i < n; i++)
		{
			MoveWindow(hQuick[i], m + i * (qw + m), quickY, qw, rowH, TRUE);
			ShowWindow(hQuick[i], SW_SHOWNA);
		}
	}
	else
	{
		for (int i = 0; i < Settings::QUICK_TEXT_COUNT; i++)
			ShowWindow(hQuick[i], SW_HIDE);
	}
	int historyH = playingY - m - y;
	if (historyH < 0) historyH = 0;
	MoveWindow(hHistory, m, y, w - 2 * m, historyH, TRUE);

	MoveWindow(hStop, w - m - btnW, playingY, btnW, rowH, TRUE);
	MoveWindow(hRepeat, w - 2 * m - 2 * btnW, playingY, btnW, rowH, TRUE);
	const int clearW = 70;
	MoveWindow(hClear, w - 3 * m - 2 * btnW - clearW, playingY, clearW, rowH, TRUE);
	int playingW = w - 5 * m - 2 * btnW - clearW;
	if (playingW < 0) playingW = 0;
	MoveWindow(hPlaying, m, playingY + 4, playingW, rowH - 6, TRUE);

	MoveWindow(hInput, m, inputY, w - 3 * m - btnW, inputH, TRUE);
	MoveWindow(hSend, w - m - btnW, inputY, btnW, inputH, TRUE);

	x = m;
	MoveWindow(hEnterSends, x, bottomRowY, 105, rowH, TRUE);
	x += 105 + m;
	MoveWindow(hSidetone, x, bottomRowY, 75, rowH, TRUE);
	x += 75;
	int tbW = w - m - x;
	if (tbW > 160) tbW = 160;
	if (tbW < 0) tbW = 0;
	MoveWindow(hSidetoneLevel, x, bottomRowY, tbW, rowH, TRUE);
}

LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_GETDLGCODE:
		return CallWindowProcW(origInputProc, hwnd, msg, wParam, lParam) | DLGC_WANTALLKEYS;
	case WM_KEYDOWN:
		if (wParam == VK_RETURN && appSettings.sendOnEnter && !(GetKeyState(VK_SHIFT) & 0x8000))
		{
			SendInput();
			return 0;
		}
		if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
		{
			SendMessageW(hwnd, EM_SETSEL, 0, -1);
			return 0;
		}
		if (wParam >= '1' && wParam < '1' + Settings::QUICK_TEXT_COUNT && (GetKeyState(VK_CONTROL) & 0x8000))
		{
			OnQuickClick(static_cast<unsigned int>(wParam - '1'));
			return 0;
		}
		if (wParam == VK_ESCAPE)
		{
			StopAll();
			return 0;
		}
		break;
	case WM_CHAR:
		// suppress beep / new line for keys handled in WM_KEYDOWN
		if ((wParam == '\r' && appSettings.sendOnEnter && !(GetKeyState(VK_SHIFT) & 0x8000)) ||
			wParam == 1 /* Ctrl+A */ || wParam == 27 /* Esc */)
			return 0;
		break;
	default:
		break;
	}
	return CallWindowProcW(origInputProc, hwnd, msg, wParam, lParam);
}

HWND CreateChild(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD exStyle, int id)
{
	HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hUiWnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInstance, NULL);
	if (h && hFont)
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), FALSE);
	return h;
}

void CreateControls(void)
{
	hVoice = CreateChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, IDC_VOICE);
	hStatus = CreateChild(L"STATIC", statusText.c_str(), SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX, 0, IDC_STATUS);
	hMenuBtn = CreateChild(L"BUTTON", L"...", BS_PUSHBUTTON, 0, IDC_MENU);
	hHistory = CreateChild(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE, IDC_HISTORY);
	SendMessageW(hHistory, EM_SETLIMITTEXT, 0, 0);
	SetWindowTextW(hHistory, historyText.c_str());
	int len = GetWindowTextLengthW(hHistory);
	SendMessageW(hHistory, EM_SETSEL, len, len);
	SendMessageW(hHistory, EM_SCROLLCARET, 0, 0);
	hPlaying = CreateChild(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX, 0, IDC_PLAYING);
	hClear = CreateChild(L"BUTTON", L"Clear log", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_CLEAR);
	hRepeat = CreateChild(L"BUTTON", L"Repeat", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_REPEAT);
	hStop = CreateChild(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_STOP);
	hInput = CreateChild(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_INPUT);
	origInputProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hInput, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
	hSend = CreateChild(L"BUTTON", L"Send", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_SEND);
	hTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hUiWnd, NULL, hInstance, NULL);
	if (hTooltip)
		SendMessageW(hTooltip, TTM_SETMAXTIPWIDTH, 0, 400);
	for (int i = 0; i < Settings::QUICK_TEXT_COUNT; i++)
	{
		hQuick[i] = CreateChild(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_QUICK_FIRST + i);
		if (hTooltip && hQuick[i])
		{
			TOOLINFOW ti;
			memset(&ti, 0, sizeof(ti));
			ti.cbSize = TTTOOLINFOW_V2_SIZE;
			ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
			ti.hwnd = hUiWnd;
			ti.uId = reinterpret_cast<UINT_PTR>(hQuick[i]);
			ti.lpszText = const_cast<wchar_t*>(L"");
			SendMessageW(hTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
		}
		UpdateQuickButton(i);
	}
	hEnterSends = CreateChild(L"BUTTON", L"[Enter] to send", BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_ENTER_SENDS);
	SendMessageW(hEnterSends, BM_SETCHECK, appSettings.sendOnEnter ? BST_CHECKED : BST_UNCHECKED, 0);
	hSidetone = CreateChild(L"BUTTON", L"Sidetone", BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_SIDETONE);
	SendMessageW(hSidetone, BM_SETCHECK, appSettings.sidetone ? BST_CHECKED : BST_UNCHECKED, 0);
	hSidetoneLevel = CreateChild(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, IDC_SIDETONE_LEVEL);
	SendMessageW(hSidetoneLevel, TBM_SETRANGE, FALSE, MAKELPARAM(0, 200));
	SendMessageW(hSidetoneLevel, TBM_SETPAGESIZE, 0, 10);
	SendMessageW(hSidetoneLevel, TBM_SETPOS, TRUE, appSettings.sidetoneLevel);
	ShowWindow(hSidetoneLevel, appSettings.sidetone ? SW_SHOW : SW_HIDE);
	FillVoiceCombo();
	UpdatePlayingUi();
	LayoutUi();
}

void ShowMenu(void)
{
	HMENU hMenu = CreatePopupMenu();
	AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, L"Settings...");
	AppendMenuW(hMenu, MF_STRING, IDM_CLEAR_HISTORY, L"Clear log");
	if (hEmbedParent == NULL)
		AppendMenuW(hMenu, MF_STRING | (appSettings.wnd.alwaysOnTop ? MF_CHECKED : 0), IDM_ALWAYS_ON_TOP, L"Always on top");
	AppendMenuW(hMenu, MF_STRING | (appSettings.showVoiceInHistory ? MF_CHECKED : 0), IDM_SHOW_VOICE, L"Show voice name in log");
	AppendMenuW(hMenu, MF_STRING | (appSettings.showQuickTexts ? MF_CHECKED : 0), IDM_SHOW_QUICK, L"Show quick text buttons");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About...");
	RECT rc;
	GetWindowRect(hMenuBtn, &rc);
	TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, rc.right, rc.bottom, 0, hUiWnd, NULL);
	DestroyMenu(hMenu);
}

void ShowAbout(void)
{
	std::wstring text = L"tSIP text-to-speech plugin " L"" PLUGIN_VERSION L"\r\n\r\n"
		L"Speech synthesis: sherpa-onnx (Apache-2.0) with onnxruntime;\r\n"
		L"voices: Piper / other models, see model licenses.\r\n\r\n"
		L"Required sherpa-onnx version: " + Utils::Utf8ToWide(T2S::SHERPA_ONNX_VERSION) + L" (32-bit)\r\n"
		L"Data directory:\r\n" + baseDir + L"\r\n\r\nConfiguration:\r\n" + configPath;
	MessageBoxW(hUiWnd, text.c_str(), L"About", MB_ICONINFORMATION);
}

void SaveWindowPosition(void)
{
	if (hUiWnd == NULL || hEmbedParent != NULL)
		return;
	WINDOWPLACEMENT wp;
	wp.length = sizeof(wp);
	if (!GetWindowPlacement(hUiWnd, &wp))
		return;
	appSettings.wnd.x = wp.rcNormalPosition.left;
	appSettings.wnd.y = wp.rcNormalPosition.top;
	appSettings.wnd.width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
	appSettings.wnd.height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
	appSettings.wnd.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
}

void DestroyUi(void)
{
	if (hUiWnd)
	{
		SaveWindowPosition();
		destroyingUi = true;
		DestroyWindow(hUiWnd);
		destroyingUi = false;
	}
	hUiWnd = NULL;
	hEmbedParent = NULL;
}

void DoShowSettings(HWND parent);

void ReadSettings(void)
{
	appSettings = Settings();
	int status = appSettings.Read(configPath);
	if (status == 1)
	{
		LOG("Config file not found, using defaults\n");
	}
	else if (status != 0)
	{
		// invalid file would be overwritten with defaults when settings are saved: keep a copy
		std::wstring copy = configPath + L".bad";
		CopyFileW(configPath.c_str(), copy.c_str(), FALSE);
		appSettings = Settings();
		LOG("Config file is invalid, using defaults; copy of the file: %s\n", Utils::WideToUtf8(copy).c_str());
	}
}

LRESULT CALLBACK UiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_SIZE:
		LayoutUi();
		return 0;
	case WM_GETMINMAXINFO:
	{
		MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO*>(lParam);
		mmi->ptMinTrackSize.x = 300;
		mmi->ptMinTrackSize.y = 250;
		return 0;
	}
	case WM_CLOSE:
		// window lives as long as plugin is enabled
		ShowWindow(hwnd, SW_MINIMIZE);
		return 0;
	case WM_EXITSIZEMOVE:
		SaveWindowPosition();
		return 0;
	case WM_CTLCOLORSTATIC:
		// read-only history: keep normal edit background
		if (reinterpret_cast<HWND>(lParam) == hHistory)
		{
			SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
			SetTextColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOWTEXT));
			return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
		}
		break;
	case WM_HSCROLL:
		if (reinterpret_cast<HWND>(lParam) == hSidetoneLevel)
		{
			int pos = static_cast<int>(SendMessageW(hSidetoneLevel, TBM_GETPOS, 0, 0));
			if (pos != appSettings.sidetoneLevel)
			{
				appSettings.sidetoneLevel = pos;
				if (appSettings.sidetone && LOWORD(wParam) != TB_THUMBTRACK)
					ApplySidetone();
			}
			if (LOWORD(wParam) == TB_ENDTRACK && appSettings.sidetone)
				ApplySidetone();
		}
		return 0;
	case WM_CONTEXTMENU:
		for (int i = 0; i < Settings::QUICK_TEXT_COUNT; i++)
		{
			if (reinterpret_cast<HWND>(wParam) == hQuick[i] && hQuick[i])
			{
				ShowQuickMenu(i, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				return 0;
			}
		}
		break;
	case WM_COMMAND:
		if (LOWORD(wParam) >= IDC_QUICK_FIRST && LOWORD(wParam) < IDC_QUICK_FIRST + Settings::QUICK_TEXT_COUNT)
		{
			OnQuickClick(LOWORD(wParam) - IDC_QUICK_FIRST);
			return 0;
		}
		switch (LOWORD(wParam))
		{
		case IDC_SEND:
			SendInput();
			SetFocus(hInput);
			return 0;
		case IDC_STOP:
			StopAll();
			return 0;
		case IDC_REPEAT:
			Repeat();
			return 0;
		case IDC_MENU:
			ShowMenu();
			return 0;
		case IDC_ENTER_SENDS:
			appSettings.sendOnEnter = SendMessageW(hEnterSends, BM_GETCHECK, 0, 0) == BST_CHECKED;
			return 0;
		case IDC_SIDETONE:
			appSettings.sidetone = SendMessageW(hSidetone, BM_GETCHECK, 0, 0) == BST_CHECKED;
			ShowWindow(hSidetoneLevel, appSettings.sidetone ? SW_SHOW : SW_HIDE);
			ApplySidetone();
			return 0;
		case IDC_VOICE:
			if (HIWORD(wParam) == CBN_SELCHANGE)
			{
				int sel = static_cast<int>(SendMessageW(hVoice, CB_GETCURSEL, 0, 0));
				if (sel >= 0 && sel < static_cast<int>(appSettings.voices.size()) && sel != appSettings.currentVoice)
				{
					appSettings.currentVoice = sel;
					PreloadCurrentVoice();
				}
			}
			return 0;
		case IDM_SETTINGS:
			DoShowSettings(hwnd);
			return 0;
		case IDM_CLEAR_HISTORY:
		case IDC_CLEAR:
			historyText.clear();
			SetWindowTextW(hHistory, L"");
			return 0;
		case IDM_ALWAYS_ON_TOP:
			appSettings.wnd.alwaysOnTop = !appSettings.wnd.alwaysOnTop;
			ApplyAlwaysOnTop();
			return 0;
		case IDM_SHOW_VOICE:
			appSettings.showVoiceInHistory = !appSettings.showVoiceInHistory;
			return 0;
		case IDM_SHOW_QUICK:
			appSettings.showQuickTexts = !appSettings.showQuickTexts;
			LayoutUi();
			return 0;
		case IDM_ABOUT:
			ShowAbout();
			return 0;
		default:
			break;
		}
		break;
	case WM_DESTROY:
		// embedded: also destroyed together with tSIP button (e.g. buttons recreated) - re-embedded by timer
		for (int i = 0; i < Settings::QUICK_TEXT_COUNT; i++)
			hQuick[i] = NULL;
		hTooltip = NULL;
		hVoice = hStatus = hMenuBtn = hHistory = hPlaying = hClear = hRepeat = hStop = hInput = hSend = hEnterSends = hSidetone = hSidetoneLevel = NULL;
		if (hwnd == hUiWnd)
		{
			hUiWnd = NULL;
			if (!destroyingUi)
				LOG("Window destroyed together with parent button, will be recreated\n");
		}
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/** Get window handle of tSIP button (Lua GetButtonHandle) */
HWND GetButtonHandle(int buttonId)
{
	char script[64];
	snprintf(script, sizeof(script), "return GetButtonHandle(%d)", buttonId);
	Json::Value params(Json::objectValue), result;
	params["script"] = script;
	params["async"] = false;
	int status = HostInvoke("RunScript", params, result);
	if (status != TSIP_OK)
		return NULL;
	std::string ret = result.get("return", "").asString();
	double val = strtod(ret.c_str(), NULL);
	if (val <= 0)
		return NULL;
	HWND hwnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(static_cast<unsigned long>(val)));
	return IsWindow(hwnd) ? hwnd : NULL;
}

void CreateUi(void)
{
	if (hUiWnd)
		return;
	if (appSettings.embedButtonId > 0)
	{
		HWND parent = GetButtonHandle(appSettings.embedButtonId);
		if (parent == NULL)
		{
			if (!embedFailedLogged)
			{
				LOG("Cannot get window of button %d for embedding (button exists? Lua GetButtonHandle available?)\n",
					appSettings.embedButtonId);
				SetStatus(L"Cannot embed window in button - check button ID");
				embedFailedLogged = true;
			}
			return;
		}
		embedFailedLogged = false;
		RECT rc;
		GetClientRect(parent, &rc);
		hEmbedParent = parent;
		hUiWnd = CreateWindowExW(WS_EX_CONTROLPARENT, UI_WND_CLASS, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, 0, rc.right, rc.bottom, parent, NULL, hInstance, NULL);
		if (hUiWnd == NULL)
		{
			hEmbedParent = NULL;
			return;
		}
		CreateControls();
		ShowWindow(hUiWnd, SW_SHOW);
		BringWindowToTop(hUiWnd);
	}
	else
	{
		// keep window on screen (e.g. monitor disconnected)
		int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
		int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		Settings::Window &wnd = appSettings.wnd;
		if (wnd.width < 300 || wnd.width > vw) wnd.width = 420;
		if (wnd.height < 220 || wnd.height > vh) wnd.height = 360;
		if (wnd.x < vx || wnd.x + 50 > vx + vw) wnd.x = vx + 30;
		if (wnd.y < vy || wnd.y + 50 > vy + vh) wnd.y = vy + 30;
		hUiWnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, UI_WND_CLASS, L"tSIP text-to-speech",
			WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, wnd.x, wnd.y, wnd.width, wnd.height, NULL, NULL, hInstance, NULL);
		if (hUiWnd == NULL)
			return;
		// position is saved from GetWindowPlacement (workspace coordinates) - restore the same way,
		// otherwise window moves by taskbar size on each start with taskbar on the top / left
		WINDOWPLACEMENT wp;
		memset(&wp, 0, sizeof(wp));
		wp.length = sizeof(wp);
		wp.showCmd = SW_HIDE;
		wp.rcNormalPosition.left = wnd.x;
		wp.rcNormalPosition.top = wnd.y;
		wp.rcNormalPosition.right = wnd.x + wnd.width;
		wp.rcNormalPosition.bottom = wnd.y + wnd.height;
		SetWindowPlacement(hUiWnd, &wp);
		HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PLUGIN));
		if (hIcon)
		{
			SendMessageW(hUiWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
			SendMessageW(hUiWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
		}
		CreateControls();
		ShowWindow(hUiWnd, wnd.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
		ApplyAlwaysOnTop();
	}
}

void CheckEmbedding(void)
{
	if (appSettings.embedButtonId <= 0)
		return;
	if (hUiWnd == NULL || !IsWindow(hUiWnd))
	{
		hUiWnd = NULL;
		CreateUi();
		return;
	}
	RECT rcParent, rcWnd;
	GetClientRect(hEmbedParent, &rcParent);
	GetClientRect(hUiWnd, &rcWnd);
	if (rcParent.right != rcWnd.right || rcParent.bottom != rcWnd.bottom)
	{
		MoveWindow(hUiWnd, 0, 0, rcParent.right, rcParent.bottom, TRUE);
		// parent (tSIP button) may paint over the area: repaint everything
		RedrawWindow(hUiWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
	}
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_APP_WORKER_RESULT:
	{
		TtsWorker::Result *r = reinterpret_cast<TtsWorker::Result*>(lParam);
		if (started)
			OnWorkerResult(r);
		delete r;
		return 0;
	}
	case WM_APP_HOST_EVENT:
	{
		std::string *s = reinterpret_cast<std::string*>(lParam);
		size_t pos = s->find('\n');
		if (started && pos != std::string::npos && s->compare(0, pos, "call_state") == 0)
		{
			Json::Reader reader;
			Json::Value jv;
			if (reader.parse(s->substr(pos + 1), jv) && jv.isObject())
				OnCallStateEvent(jv);
		}
		delete s;
		return 0;
	}
	case WM_TIMER:
		if (wParam == TIMER_PLAYBACK)
			CheckPlayback();
		else if (wParam == TIMER_EMBED)
			CheckEmbedding();
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void DrainMessages(void)
{
	// results / events not processed yet: free memory
	MSG m;
	while (PeekMessageW(&m, hMsgWnd, WM_APP_WORKER_RESULT, WM_APP_WORKER_RESULT, PM_REMOVE))
		delete reinterpret_cast<TtsWorker::Result*>(m.lParam);
	while (PeekMessageW(&m, hMsgWnd, WM_APP_HOST_EVENT, WM_APP_HOST_EVENT, PM_REMOVE))
		delete reinterpret_cast<std::string*>(m.lParam);
}

bool RegisterClasses(void)
{
	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = MsgWndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = MSG_WND_CLASS;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;

	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = UiWndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	wc.lpszClassName = UI_WND_CLASS;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;
	return true;
}

void UnregisterClasses(void)
{
	UnregisterClassW(MSG_WND_CLASS, hInstance);
	UnregisterClassW(UI_WND_CLASS, hInstance);
}

std::wstring GetProfileDir(void)
{
	Json::Value result;
	if (HostInvoke("GetAppInfo", Json::Value(Json::objectValue), result) == TSIP_OK &&
		result["profileDir"].isString())
	{
		std::wstring dir = Utils::Utf8ToWide(result["profileDir"].asString());
		while (!dir.empty() && (dir[dir.size() - 1] == L'\\' || dir[dir.size() - 1] == L'/'))
			dir.erase(dir.size() - 1);
		return dir;
	}
	return std::wstring();
}

void InitPaths(void)
{
	hInstance = Utils::GetCurrentModule();
	dllName = Utils::GetDllNameWithoutExtension();
	baseDir = Utils::GetDllDir() + L"\\" + dllName;
	profileDir = GetProfileDir();
	if (!profileDir.empty())
		configPath = profileDir + L"\\" + dllName + L".cfg";
	else
		configPath = Utils::ReplaceFileExtension(Utils::GetDllPath(), L".cfg");
}

void WriteSilenceFile(void)
{
	std::vector<short> pcm(16000 / 5, 0);	// 200 ms
	std::vector<char> wav;
	T2S::MakeWav(pcm, 16000, wav);
	if (profileDir.empty() || T2S::WriteFile(profileDir + L"\\" + SilenceFileName(), wav) != 0)
		LOG("Failed to write silence WAV file to profile directory\n");
}

bool EngineSettingsChanged(const Settings &a, const Settings &b)
{
	if (a.libraryDir != b.libraryDir || a.numThreads != b.numThreads || a.voices.size() != b.voices.size())
		return true;
	for (unsigned int i = 0; i < a.voices.size(); i++)
	{
		if (!a.voices[i].SameModel(b.voices[i]))
			return true;
	}
	return false;
}

void DoShowSettings(HWND parent)
{
	Settings edited = appSettings;
	// opened from tSIP settings: plugin window would stay usable and its changes would be overwritten
	bool uiDisabled = (hUiWnd != NULL && parent != hUiWnd && IsWindowEnabled(hUiWnd));
	if (uiDisabled)
		EnableWindow(hUiWnd, FALSE);
	bool ok = SettingsDialog::Show(hInstance, parent, edited, baseDir);
	if (uiDisabled && hUiWnd)
		EnableWindow(hUiWnd, TRUE);
	if (!ok)
		return;

	if (!started)
	{
		appSettings = edited;
		if (appSettings.Write(configPath) != 0)
			MessageBoxW(parent, (L"Failed to save " + configPath).c_str(), L"TTS plugin settings", MB_ICONERROR);
		return;
	}

	SaveWindowPosition();
	bool engineChanged = EngineSettingsChanged(appSettings, edited);
	bool embedChanged = (appSettings.embedButtonId != edited.embedButtonId);
	bool voiceChanged = engineChanged || (appSettings.currentVoice != edited.currentVoice);
	bool silenceChanged = (appSettings.silenceMicrophone != edited.silenceMicrophone);
	// window state is owned by the running window
	edited.wnd.x = appSettings.wnd.x;
	edited.wnd.y = appSettings.wnd.y;
	edited.wnd.width = appSettings.wnd.width;
	edited.wnd.height = appSettings.wnd.height;
	edited.wnd.maximized = appSettings.wnd.maximized;
	appSettings = edited;
	appSettings.Write(configPath);

	if (engineChanged)
	{
		StopAll();
		TtsWorker::Request req;
		req.type = TtsWorker::Request::RESET;
		TtsWorker::Post(req);
	}
	if (voiceChanged)
		PreloadCurrentVoice();
	if (silenceChanged)
	{
		// apply to calls in progress (except call with message being played - applied after message)
		for (std::set<unsigned int>::const_iterator it = establishedCalls.begin(); it != establishedCalls.end(); ++it)
		{
			if (!(playMode == PLAY_CALL && playCallUid == *it))
				SetIdleAudioInput(*it);
		}
	}

	if (embedChanged)
	{
		// may be called from menu of the window being destroyed: recreate later
		embedFailedLogged = false;
		DestroyUi();
		CreateUi();
	}
	else if (hUiWnd)
	{
		FillVoiceCombo();
		SendMessageW(hEnterSends, BM_SETCHECK, appSettings.sendOnEnter ? BST_CHECKED : BST_UNCHECKED, 0);
		ApplyAlwaysOnTop();
	}
}

}	// namespace

int Plugin::Start(void)
{
	if (started)
		return 0;
	InitPaths();
	ReadSettings();
	if (appSettings.voices.empty())
	{
		// first start (or all voices removed): use voices present in data directory
		appSettings.voices = Voices::FindInstalled(baseDir);
		appSettings.currentVoice = 0;
		if (!appSettings.voices.empty())
			LOG("Found %u installed voice(s)\n", static_cast<unsigned int>(appSettings.voices.size()));
	}

	INITCOMMONCONTROLSEX icc;
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);

	if (!RegisterClasses())
	{
		LOG("Failed to register window classes\n");
		return -1;
	}
	hMsgWnd = CreateWindowExW(0, MSG_WND_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
	if (hMsgWnd == NULL)
	{
		LOG("Failed to create message window\n");
		UnregisterClasses();
		return -1;
	}

	NONCLIENTMETRICSW ncm;
	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
		hFont = CreateFontIndirectW(&ncm.lfMessageFont);

	if (profileDir.empty())
		LOG("Cannot get tSIP profile directory - speech cannot be played into calls\n");
	WriteSilenceFile();

	started = true;
	workerRunning = (TtsWorker::Start(hMsgWnd, WM_APP_WORKER_RESULT) == 0);
	if (!workerRunning)
		LOG("Failed to start speech engine thread\n");
	statusText.clear();
	PreloadCurrentVoice();
	CreateUi();
	SetTimer(hMsgWnd, TIMER_EMBED, EMBED_CHECK_MS, NULL);

	// plugin enabled during call: same as if call was just established
	Json::Value calls;
	if (HostInvoke("GetCalls", Json::Value(Json::objectValue), calls) == TSIP_OK && calls["calls"].isArray())
	{
		for (unsigned int i = 0; i < calls["calls"].size(); i++)
		{
			if (calls["calls"][i].isObject())
				OnCallStateEvent(calls["calls"][i]);
		}
	}
	LOG("Started, data directory: %s, config: %s\n", Utils::WideToUtf8(baseDir).c_str(), Utils::WideToUtf8(configPath).c_str());
	return 0;
}

void Plugin::Stop(void)
{
	if (!started)
		return;
	started = false;
	TtsWorker::Stop();
	pendingSpeech.clear();
	playQueue.clear();
	if (playMode == PLAY_LOCAL)
		PlaySoundW(NULL, NULL, 0);
	localWav.clear();
	playMode = PLAY_NONE;

	// give microphone back to calls, switch off sidetone enabled by plugin
	for (std::set<unsigned int>::const_iterator it = touchedCalls.begin(); it != touchedCalls.end(); ++it)
		RestoreMicrophone(*it);
	touchedCalls.clear();
	std::set<unsigned int> sidetoneUids = sidetoneCalls;
	for (std::set<unsigned int>::const_iterator it = sidetoneUids.begin(); it != sidetoneUids.end(); ++it)
		SetSidetone(*it, false);
	sidetoneCalls.clear();
	establishedCalls.clear();
	workerRunning = false;

	DestroyUi();
	if (hMsgWnd)
	{
		KillTimer(hMsgWnd, TIMER_PLAYBACK);
		KillTimer(hMsgWnd, TIMER_EMBED);
		DrainMessages();
		DestroyWindow(hMsgWnd);
		hMsgWnd = NULL;
	}
	if (hFont)
	{
		DeleteObject(hFont);
		hFont = NULL;
	}
	UnregisterClasses();
	if (appSettings.Write(configPath) != 0)
		LOG("Failed to save settings\n");
	haveLastItem = false;
	lastItem = Item();
	current = Item();
	LOG("Stopped\n");
}

void Plugin::OnEvent(const char *type, const char *json)
{
	// handled asynchronously: plugin must not block in OnEvent, Invoke() is safer outside of event dispatch
	if (!started || hMsgWnd == NULL)
		return;
	if (strcmp(type, "call_state") != 0)
		return;
	std::string *s = new std::string(type);
	*s += '\n';
	*s += json;
	if (!PostMessageW(hMsgWnd, WM_APP_HOST_EVENT, 0, reinterpret_cast<LPARAM>(s)))
		delete s;
}

void Plugin::ShowSettings(HWND parent)
{
	if (!started)
	{
		InitPaths();
		ReadSettings();
	}
	DoShowSettings(parent);
}
