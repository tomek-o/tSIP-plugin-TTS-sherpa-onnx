#include "TtsWorker.h"
#include "Log.h"
#include "Utils.h"
#include <float.h>
#include <deque>
#include <list>

using namespace TtsWorker;

namespace
{

/** Loaded models kept in memory: switching between e.g. two languages does not require reloading */
enum { MAX_LOADED_ENGINES = 2 };

struct LoadedEngine
{
	std::wstring key;
	T2S::Engine *engine;
};

HANDLE hThread = NULL;
HANDLE hEvent = NULL;		///< auto-reset: new request or stop
CRITICAL_SECTION cs;
bool csInitialized = false;
std::deque<Request> requests;
volatile LONG stopRequested = 0;
volatile long cancelSpeech = 0;		///< cancel speech being generated; reset when next SPEAK request is taken
int speechInProgress = 0;			///< SPEAK request being processed (under cs)
HWND hNotifyWnd = NULL;
UINT notifyMsg = 0;
/** worker did not stop in time (e.g. stuck in model loading): it may still run code of this DLL */
bool workerAbandoned = false;

/** used only by worker thread */
std::list<LoadedEngine> engines;	///< most recently used first
std::wstring loadedLibraryDir;

void PostResult(Result *result)
{
	if (stopRequested || !PostMessage(hNotifyWnd, notifyMsg, 0, reinterpret_cast<LPARAM>(result)))
		delete result;
}

void DestroyEngines(void)
{
	for (std::list<LoadedEngine>::iterator it = engines.begin(); it != engines.end(); ++it)
		delete it->engine;
	engines.clear();
}

/** Find or create engine for the configuration */
T2S::Engine* GetEngine(const T2S::EngineConfig &cfg, std::string &error)
{
	if (T2S::IsLibraryLoaded() && cfg.libraryDir != loadedLibraryDir)
	{
		DestroyEngines();
		T2S::UnloadSherpaLibrary();
	}
	if (!T2S::IsLibraryLoaded())
	{
		if (T2S::LoadSherpaLibrary(cfg.libraryDir, error) != 0)
			return NULL;
		loadedLibraryDir = cfg.libraryDir;
	}

	std::wstring key = cfg.Key();
	for (std::list<LoadedEngine>::iterator it = engines.begin(); it != engines.end(); ++it)
	{
		if (it->key == key)
		{
			engines.splice(engines.begin(), engines, it);
			return engines.front().engine;
		}
	}

	while (engines.size() >= MAX_LOADED_ENGINES)
	{
		delete engines.back().engine;
		engines.pop_back();
	}
	T2S::Engine *engine = new T2S::Engine();
	if (engine->Create(cfg, error) != 0)
	{
		delete engine;
		return NULL;
	}
	LoadedEngine le;
	le.key = key;
	le.engine = engine;
	engines.push_front(le);
	return engine;
}

void Process(const Request &req)
{
	Result *result = new Result();
	result->type = req.type;
	result->id = req.id;
	DWORD start = GetTickCount();

	switch (req.type)
	{
	case Request::RESET:
		DestroyEngines();
		T2S::UnloadSherpaLibrary();
		loadedLibraryDir.clear();
		break;
	case Request::LOAD:
	{
		T2S::Engine *engine = GetEngine(req.engine, result->error);
		result->status = engine ? 0 : 2;
		if (engine)
			result->sampleRate = engine->GetSampleRate();
		break;
	}
	case Request::SPEAK:
	{
		T2S::Engine *engine = GetEngine(req.engine, result->error);
		if (engine == NULL)
		{
			result->status = 2;
			break;
		}
		if (cancelSpeech)
		{
			result->status = 1;	// cancelled while model was loading
			break;
		}
		result->status = engine->Generate(req.textUtf8, req.speakerId, req.speed, &cancelSpeech, result->pcm, result->error);
		result->sampleRate = engine->GetSampleRate();
		break;
	}
	}

	result->elapsedMs = GetTickCount() - start;
	if (result->status > 1)
		LOG("%s\n", result->error.c_str());
	PostResult(result);
}

DWORD WINAPI ThreadProc(LPVOID)
{
	// host (Borland C++ Builder application) unmasks FPU exceptions; this is a new thread with default
	// control word, but make sure onnxruntime math does not raise floating point exceptions
	_controlfp(_MCW_EM, _MCW_EM);

	while (!stopRequested)
	{
		Request req;
		bool haveRequest = false;
		EnterCriticalSection(&cs);
		if (!requests.empty())
		{
			req = requests.front();
			requests.pop_front();
			haveRequest = true;
			if (req.type == Request::SPEAK)
			{
				speechInProgress = 1;
				// requests queued before CancelSpeech() were removed - this one is newer
				InterlockedExchange(&cancelSpeech, 0);
			}
		}
		LeaveCriticalSection(&cs);

		if (!haveRequest)
		{
			WaitForSingleObject(hEvent, INFINITE);
			continue;
		}
		Process(req);

		if (req.type == Request::SPEAK)
		{
			EnterCriticalSection(&cs);
			speechInProgress = 0;
			LeaveCriticalSection(&cs);
		}
	}

	DestroyEngines();
	T2S::UnloadSherpaLibrary();
	loadedLibraryDir.clear();
	return 0;
}

}	// namespace

int TtsWorker::Start(HWND hwnd, UINT msg)
{
	if (hThread)
		return 0;
	if (workerAbandoned)
	{
		LOG("Previous worker thread still running - restart tSIP\n");
		return 3;
	}
	if (!csInitialized)
	{
		InitializeCriticalSection(&cs);
		csInitialized = true;
	}
	hNotifyWnd = hwnd;
	notifyMsg = msg;
	stopRequested = 0;
	cancelSpeech = 0;
	speechInProgress = 0;
	requests.clear();
	hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (hEvent == NULL)
		return 1;
	DWORD threadId;
	hThread = CreateThread(NULL, 0, ThreadProc, NULL, 0, &threadId);
	if (hThread == NULL)
	{
		CloseHandle(hEvent);
		hEvent = NULL;
		return 2;
	}
	return 0;
}

void TtsWorker::Stop(void)
{
	if (hThread == NULL)
		return;
	EnterCriticalSection(&cs);
	requests.clear();
	LeaveCriticalSection(&cs);
	InterlockedExchange(&cancelSpeech, 1);
	InterlockedExchange(&stopRequested, 1);
	SetEvent(hEvent);
	// model loading cannot be interrupted - may take few seconds
	if (WaitForSingleObject(hThread, 20000) != WAIT_OBJECT_0)
	{
		// thread still uses this DLL, its state and the event: keep DLL loaded (pinned) until process exit,
		// do not reuse state - new thread is not started
		LOG("Worker thread did not stop in time - plugin DLL stays loaded, restart tSIP to use plugin again\n");
		HMODULE hModule;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&TtsWorker::Stop), &hModule);
		workerAbandoned = true;
		CloseHandle(hThread);
		hThread = NULL;
		return;
	}
	CloseHandle(hThread);
	hThread = NULL;
	CloseHandle(hEvent);
	hEvent = NULL;
	hNotifyWnd = NULL;
}

void TtsWorker::Post(const Request &request)
{
	if (hThread == NULL)
		return;
	EnterCriticalSection(&cs);
	requests.push_back(request);
	LeaveCriticalSection(&cs);
	SetEvent(hEvent);
}

void TtsWorker::CancelSpeech(void)
{
	if (hThread == NULL)
		return;
	EnterCriticalSection(&cs);
	bool anySpeech = speechInProgress != 0;
	for (std::deque<Request>::iterator it = requests.begin(); it != requests.end(); )
	{
		if (it->type == Request::SPEAK)
		{
			it = requests.erase(it);
		}
		else
		{
			++it;
		}
	}
	if (anySpeech)
		InterlockedExchange(&cancelSpeech, 1);
	LeaveCriticalSection(&cs);
}

int TtsWorker::GetPendingSpeechCount(void)
{
	if (hThread == NULL)
		return 0;
	int count = 0;
	EnterCriticalSection(&cs);
	count = speechInProgress;
	for (std::deque<Request>::const_iterator it = requests.begin(); it != requests.end(); ++it)
	{
		if (it->type == Request::SPEAK)
			count++;
	}
	LeaveCriticalSection(&cs);
	return count;
}
