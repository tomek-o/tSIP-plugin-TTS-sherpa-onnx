#include "T2S.h"
#include "Utils.h"
#include "Log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "c-api.h"

const char* const T2S::SHERPA_ONNX_VERSION = "1.13.8";

namespace
{

const wchar_t* const SHERPA_DLL_NAME = L"sherpa-onnx-c-api.dll";

typedef decltype(&SherpaOnnxGetVersionStr) pfGetVersionStr;
typedef decltype(&SherpaOnnxCreateOfflineTts) pfCreateOfflineTts;
typedef decltype(&SherpaOnnxDestroyOfflineTts) pfDestroyOfflineTts;
typedef decltype(&SherpaOnnxOfflineTtsSampleRate) pfOfflineTtsSampleRate;
typedef decltype(&SherpaOnnxOfflineTtsNumSpeakers) pfOfflineTtsNumSpeakers;
typedef decltype(&SherpaOnnxOfflineTtsGenerateWithConfig) pfOfflineTtsGenerateWithConfig;
typedef decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio) pfDestroyOfflineTtsGeneratedAudio;

struct SherpaApi
{
	HMODULE hModule;
	pfGetVersionStr GetVersionStr;
	pfCreateOfflineTts CreateOfflineTts;
	pfDestroyOfflineTts DestroyOfflineTts;
	pfOfflineTtsSampleRate OfflineTtsSampleRate;
	pfOfflineTtsNumSpeakers OfflineTtsNumSpeakers;
	pfOfflineTtsGenerateWithConfig OfflineTtsGenerateWithConfig;
	pfDestroyOfflineTtsGeneratedAudio DestroyOfflineTtsGeneratedAudio;
} api;

template<typename T>
bool GetFn(T &fn, const char *name, std::string &missing)
{
	fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(api.hModule, name)));
	if (fn == NULL)
	{
		if (!missing.empty())
			missing += ", ";
		missing += name;
		return false;
	}
	return true;
}

/** sherpa-onnx (MSVC build) opens files using narrow strings in ANSI code page */
bool ToAnsiPath(const std::wstring &path, std::string &out, const char *what, std::string &error)
{
	if (!Utils::IsAnsiRepresentable(path))
	{
		error = std::string(what) + ": path contains characters not supported by sherpa-onnx (outside system code page): " +
			Utils::WideToUtf8(path);
		return false;
	}
	out = Utils::WideToAnsi(path);
	return true;
}

bool CheckFile(const std::wstring &path, const char *what, bool required, std::string &error)
{
	if (path.empty())
	{
		if (required)
		{
			error = std::string(what) + " not configured";
			return false;
		}
		return true;
	}
	if (!Utils::FileExists(path))
	{
		error = std::string(what) + " not found: " + Utils::WideToUtf8(path);
		return false;
	}
	return true;
}

bool CheckDir(const std::wstring &path, const char *what, std::string &error)
{
	if (path.empty())
		return true;
	if (!Utils::DirectoryExists(path))
	{
		error = std::string(what) + " not found: " + Utils::WideToUtf8(path);
		return false;
	}
	return true;
}

/*
 * Exceptions escaping from sherpa-onnx C API.
 * sherpa-onnx / onnxruntime (MSVC) may throw C++ exception through C API, e.g. for model not supported
 * by this onnxruntime build (seen with fp16 Piper models). Such exception is not catchable by this
 * (MinGW, DWARF exceptions) code and would terminate tSIP. Calls are guarded with SEH frame: unhandled
 * exception is turned into an error. Memory allocated by the failed call may leak.
 * Single thread only (T2S is used by worker thread).
 */
#if defined(__i386__)
struct SehRegistration
{
	SehRegistration *prev;
	void *handler;
};

void *guardJmpBuf[5];
DWORD guardExceptionCode = 0;

const DWORD MSVC_CPP_EXCEPTION = 0xE06D7363;

EXCEPTION_DISPOSITION __cdecl GuardHandler(PEXCEPTION_RECORD rec, void *frame, PCONTEXT, void *)
{
	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
		return ExceptionContinueSearch;
	// only C++ exceptions thrown by sherpa-onnx / onnxruntime; hardware exceptions (access violation,
	// stack overflow, ...) mean corrupted state - not hidden
	if (rec->ExceptionCode != MSVC_CPP_EXCEPTION)
		return ExceptionContinueSearch;
	guardExceptionCode = rec->ExceptionCode;
	// unwind frames between the throw and this guard: destructors of sherpa-onnx / onnxruntime objects
	// run (locks released, partially created session and its threads destroyed);
	// fs:0 is set back to this guard's registration
	RtlUnwind(frame, NULL, rec, NULL);
	__builtin_longjmp(guardJmpBuf, 1);
}
#endif

/** Exception caught in the library: its state may be inconsistent - never unloaded (FreeLibrary) */
bool libraryFaulted = false;

/** Call fn(arg) with SEH guard
	\return true if function returned normally, false if exception occurred (code: exceptionCode)
*/
__attribute__((noinline)) bool GuardedCall(void (*fn)(void*), void *arg, DWORD &exceptionCode)
{
#if defined(__i386__)
	volatile SehRegistration reg;
	SehRegistration *prev;
	__asm__ __volatile__ ("movl %%fs:0, %0" : "=r" (prev));
	reg.prev = prev;
	reg.handler = reinterpret_cast<void*>(GuardHandler);
	bool ok;
	if (__builtin_setjmp(guardJmpBuf) == 0)
	{
		__asm__ __volatile__ ("movl %0, %%fs:0" : : "r" (&reg) : "memory");
		fn(arg);
		ok = true;
	}
	else
	{
		exceptionCode = guardExceptionCode;
		libraryFaulted = true;
		ok = false;
	}
	__asm__ __volatile__ ("movl %0, %%fs:0" : : "r" (prev) : "memory");
	return ok;
#else
	fn(arg);
	(void)exceptionCode;
	return true;
#endif
}

std::string ExceptionText(DWORD code)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "exception 0x%08lX in sherpa-onnx%s", code,
		code == 0xE06D7363 ? " (C++ exception - model not supported by this sherpa-onnx / onnxruntime build?)" : "");
	return buf;
}

struct CreateCall
{
	const SherpaOnnxOfflineTtsConfig *config;
	const SherpaOnnxOfflineTts *result;
	static void Fn(void *arg)
	{
		CreateCall *c = static_cast<CreateCall*>(arg);
		c->result = api.CreateOfflineTts(c->config);
	}
};

struct GenerateCall
{
	const SherpaOnnxOfflineTts *tts;
	const char *text;
	const SherpaOnnxGenerationConfig *config;
	SherpaOnnxGeneratedAudioProgressCallbackWithArg callback;
	void *arg;
	const SherpaOnnxGeneratedAudio *result;
	static void Fn(void *a)
	{
		GenerateCall *c = static_cast<GenerateCall*>(a);
		c->result = api.OfflineTtsGenerateWithConfig(c->tts, c->text, c->config, c->callback, c->arg);
	}
};

struct ProgressArg
{
	volatile long *cancel;
};

int32_t ProgressCallback(const float *samples, int32_t n, float p, void *arg)
{
	(void)samples;
	(void)n;
	(void)p;
	ProgressArg *pa = static_cast<ProgressArg*>(arg);
	if (pa && pa->cancel && *pa->cancel)
		return 0;	// stop
	return 1;
}

}	// namespace

using namespace T2S;

std::wstring EngineConfig::Key(void) const
{
	wchar_t buf[32];
	swprintf(buf, sizeof(buf)/sizeof(buf[0]), L"%d", numThreads);
	return Utils::Utf8ToWide(modelType) + L"|" + model + L"|" + tokens + L"|" + dataDir + L"|" + lexicon + L"|" +
		voices + L"|" + Utils::Utf8ToWide(lang) + L"|" + buf;
}

int T2S::LoadSherpaLibrary(const std::wstring &dir, std::string &error)
{
	if (api.hModule)
		return 0;
	std::wstring path = Utils::ResolvePath(dir, SHERPA_DLL_NAME);
	if (!Utils::FileExists(path))
	{
		error = "sherpa-onnx library not found: " + Utils::WideToUtf8(path);
		return 1;
	}
	// onnxruntime.dll must be the one distributed with sherpa-onnx: if it is missing in the directory,
	// system copy (e.g. System32) could be loaded; if other onnxruntime.dll is already loaded by the
	// process (other plugin), it would be used by sherpa-onnx
	std::wstring ortPath = Utils::ResolvePath(dir, L"onnxruntime.dll");
	if (!Utils::FileExists(ortPath))
	{
		error = "onnxruntime.dll not found: " + Utils::WideToUtf8(ortPath);
		return 1;
	}
	HMODULE hOrt = GetModuleHandleW(L"onnxruntime.dll");
	if (hOrt)
	{
		wchar_t loaded[MAX_PATH * 2] = L"";
		GetModuleFileNameW(hOrt, loaded, sizeof(loaded)/sizeof(loaded[0]));
		wchar_t full[MAX_PATH * 2] = L"";
		GetFullPathNameW(ortPath.c_str(), sizeof(full)/sizeof(full[0]), full, NULL);
		if (lstrcmpiW(loaded, full) != 0)
		{
			error = "other onnxruntime.dll is already loaded by the process (" + Utils::WideToUtf8(loaded) +
				") - incompatible with sherpa-onnx " + SHERPA_ONNX_VERSION;
			return 1;
		}
	}
	// LOAD_WITH_ALTERED_SEARCH_PATH: dependencies (onnxruntime.dll) are searched in the directory of the loaded DLL
	HMODULE hModule = LoadLibraryExW(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (hModule == NULL)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), " (error %lu)", GetLastError());
		error = "failed to load " + Utils::WideToUtf8(path) + buf +
			" - onnxruntime.dll should be in the same directory, 32-bit DLLs are required";
		return 2;
	}
	memset(&api, 0, sizeof(api));
	api.hModule = hModule;
	std::string missing;
	GetFn(api.GetVersionStr, "SherpaOnnxGetVersionStr", missing);
	GetFn(api.CreateOfflineTts, "SherpaOnnxCreateOfflineTts", missing);
	GetFn(api.DestroyOfflineTts, "SherpaOnnxDestroyOfflineTts", missing);
	GetFn(api.OfflineTtsSampleRate, "SherpaOnnxOfflineTtsSampleRate", missing);
	GetFn(api.OfflineTtsNumSpeakers, "SherpaOnnxOfflineTtsNumSpeakers", missing);
	GetFn(api.OfflineTtsGenerateWithConfig, "SherpaOnnxOfflineTtsGenerateWithConfig", missing);
	GetFn(api.DestroyOfflineTtsGeneratedAudio, "SherpaOnnxDestroyOfflineTtsGeneratedAudio", missing);
	if (!missing.empty())
	{
		error = "sherpa-onnx library: missing functions (version " + std::string(SHERPA_ONNX_VERSION) +
			" with TTS support is required): " + missing;
		FreeLibrary(hModule);
		memset(&api, 0, sizeof(api));
		return 3;
	}
	std::string version = GetLibraryVersion();
	if (version != SHERPA_ONNX_VERSION)
	{
		// different version might have different structure layout - refuse to avoid crash
		error = "sherpa-onnx library version " + version + " is not supported, version " + SHERPA_ONNX_VERSION + " is required";
		FreeLibrary(hModule);
		memset(&api, 0, sizeof(api));
		return 4;
	}
	LOG("Loaded %s, version %s\n", Utils::WideToUtf8(path).c_str(), version.c_str());
	return 0;
}

void T2S::UnloadSherpaLibrary(void)
{
	if (api.hModule)
	{
		if (libraryFaulted)
			LOG("sherpa-onnx library is not unloaded (exception occurred in it)\n");
		else
			FreeLibrary(api.hModule);
		memset(&api, 0, sizeof(api));
	}
}

bool T2S::IsLibraryLoaded(void)
{
	return api.hModule != NULL;
}

std::string T2S::GetLibraryVersion(void)
{
	if (api.GetVersionStr == NULL)
		return std::string();
	const char *v = api.GetVersionStr();
	return v ? v : "";
}

Engine::Engine(void):
	tts(NULL),
	sampleRate(0),
	numSpeakers(0)
{
}

Engine::~Engine(void)
{
	Destroy();
}

int Engine::Create(const EngineConfig &cfg, std::string &error)
{
	Destroy();
	if (!IsLibraryLoaded())
	{
		error = "sherpa-onnx library not loaded";
		return 1;
	}

	bool kokoro = (cfg.modelType == "kokoro");
	bool kitten = (cfg.modelType == "kitten");
	bool vits = (cfg.modelType == "vits" || cfg.modelType.empty());
	if (!kokoro && !kitten && !vits)
	{
		error = "unsupported model type: " + cfg.modelType;
		return 1;
	}

	if (!CheckFile(cfg.model, "model", true, error) ||
		!CheckFile(cfg.tokens, "tokens", true, error) ||
		!CheckDir(cfg.dataDir, "espeak-ng data directory", error) ||
		!CheckFile(cfg.lexicon, "lexicon", false, error) ||
		!CheckFile(cfg.voices, "voices file", kokoro || kitten, error))
	{
		return 1;
	}

	std::string model, tokens, dataDir, lexicon, voices;
	if (!ToAnsiPath(cfg.model, model, "model", error) ||
		!ToAnsiPath(cfg.tokens, tokens, "tokens", error) ||
		!ToAnsiPath(cfg.dataDir, dataDir, "espeak-ng data directory", error) ||
		!ToAnsiPath(cfg.lexicon, lexicon, "lexicon", error) ||
		!ToAnsiPath(cfg.voices, voices, "voices file", error))
	{
		return 1;
	}

	SherpaOnnxOfflineTtsConfig config;
	memset(&config, 0, sizeof(config));
	if (kokoro)
	{
		config.model.kokoro.model = model.c_str();
		config.model.kokoro.voices = voices.c_str();
		config.model.kokoro.tokens = tokens.c_str();
		config.model.kokoro.data_dir = dataDir.c_str();
		config.model.kokoro.lexicon = lexicon.c_str();
		config.model.kokoro.lang = cfg.lang.c_str();
		config.model.kokoro.length_scale = 1.0f;
	}
	else if (kitten)
	{
		config.model.kitten.model = model.c_str();
		config.model.kitten.voices = voices.c_str();
		config.model.kitten.tokens = tokens.c_str();
		config.model.kitten.data_dir = dataDir.c_str();
		config.model.kitten.length_scale = 1.0f;
	}
	else
	{
		config.model.vits.model = model.c_str();
		config.model.vits.tokens = tokens.c_str();
		config.model.vits.data_dir = dataDir.c_str();
		config.model.vits.lexicon = lexicon.c_str();
		config.model.vits.noise_scale = 0.667f;
		config.model.vits.noise_scale_w = 0.8f;
		config.model.vits.length_scale = 1.0f;
	}
	config.model.num_threads = cfg.numThreads > 0 ? cfg.numThreads : 1;
	config.model.debug = 0;
	config.model.provider = "cpu";
	config.max_num_sentences = 1;	// progress callback (cancel check) after each sentence
	config.silence_scale = 0.2f;

	DWORD start = GetTickCount();
	CreateCall call;
	call.config = &config;
	call.result = NULL;
	DWORD exceptionCode = 0;
	if (!GuardedCall(CreateCall::Fn, &call, exceptionCode))
	{
		error = "failed to load model " + Utils::WideToUtf8(cfg.model) + ": " + ExceptionText(exceptionCode);
		return 3;
	}
	tts = call.result;
	if (tts == NULL)
	{
		error = "failed to create TTS from model " + Utils::WideToUtf8(cfg.model) + " (wrong model type or files?)";
		return 2;
	}
	sampleRate = api.OfflineTtsSampleRate(static_cast<const SherpaOnnxOfflineTts*>(tts));
	numSpeakers = api.OfflineTtsNumSpeakers(static_cast<const SherpaOnnxOfflineTts*>(tts));
	LOG("Model %s loaded in %lu ms, sample rate %d Hz, %d speaker(s)\n", Utils::WideToUtf8(cfg.model).c_str(),
		GetTickCount() - start, sampleRate, numSpeakers);
	return 0;
}

void Engine::Destroy(void)
{
	if (tts)
	{
		if (api.DestroyOfflineTts)
			api.DestroyOfflineTts(static_cast<const SherpaOnnxOfflineTts*>(tts));
		tts = NULL;
	}
	sampleRate = 0;
	numSpeakers = 0;
}

int Engine::Generate(const std::string &textUtf8, int speakerId, float speed, volatile long *cancel,
	std::vector<short> &pcm, std::string &error)
{
	pcm.clear();
	if (tts == NULL)
	{
		error = "TTS engine not created";
		return 2;
	}
	if (speakerId < 0 || (numSpeakers > 0 && speakerId >= numSpeakers))
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "speaker ID %d out of range (model has %d speaker(s))", speakerId, numSpeakers);
		error = buf;
		return 2;
	}

	SherpaOnnxGenerationConfig genConfig;
	memset(&genConfig, 0, sizeof(genConfig));
	genConfig.silence_scale = 0.2f;
	genConfig.speed = speed > 0 ? speed : 1.0f;
	genConfig.sid = speakerId;

	ProgressArg arg;
	arg.cancel = cancel;
	GenerateCall call;
	call.tts = static_cast<const SherpaOnnxOfflineTts*>(tts);
	call.text = textUtf8.c_str();
	call.config = &genConfig;
	call.callback = ProgressCallback;
	call.arg = &arg;
	call.result = NULL;
	DWORD exceptionCode = 0;
	if (!GuardedCall(GenerateCall::Fn, &call, exceptionCode))
	{
		error = "speech generation failed: " + ExceptionText(exceptionCode);
		return 2;
	}
	const SherpaOnnxGeneratedAudio *audio = call.result;
	if (cancel && *cancel)
	{
		if (audio)
			api.DestroyOfflineTtsGeneratedAudio(audio);
		return 1;
	}
	if (audio == NULL)
	{
		error = "speech generation failed";
		return 2;
	}
	if (audio->n <= 0 || audio->samples == NULL)
	{
		api.DestroyOfflineTtsGeneratedAudio(audio);
		error = "no audio generated (text without speakable characters?)";
		return 2;
	}
	pcm.resize(audio->n);
	for (int32_t i = 0; i < audio->n; i++)
	{
		float s = audio->samples[i] * 32767.0f;
		if (s > 32767.0f)
			s = 32767.0f;
		else if (s < -32768.0f)
			s = -32768.0f;
		pcm[i] = static_cast<short>(s);
	}
	if (audio->sample_rate > 0)
		sampleRate = audio->sample_rate;
	api.DestroyOfflineTtsGeneratedAudio(audio);
	return 0;
}

namespace
{

void Put16(std::vector<char> &v, unsigned int val)
{
	v.push_back(static_cast<char>(val & 0xFF));
	v.push_back(static_cast<char>((val >> 8) & 0xFF));
}

void Put32(std::vector<char> &v, unsigned int val)
{
	Put16(v, val & 0xFFFF);
	Put16(v, (val >> 16) & 0xFFFF);
}

void PutTag(std::vector<char> &v, const char *tag)
{
	v.insert(v.end(), tag, tag + 4);
}

}	// namespace

void T2S::MakeWav(const std::vector<short> &pcm, int sampleRate, std::vector<char> &wav)
{
	unsigned int dataSize = static_cast<unsigned int>(pcm.size() * 2);
	wav.clear();
	wav.reserve(44 + dataSize);
	PutTag(wav, "RIFF");
	Put32(wav, 36 + dataSize);
	PutTag(wav, "WAVE");
	PutTag(wav, "fmt ");
	Put32(wav, 16);
	Put16(wav, 1);					// PCM
	Put16(wav, 1);					// mono
	Put32(wav, sampleRate);
	Put32(wav, sampleRate * 2);		// byte rate
	Put16(wav, 2);					// block align
	Put16(wav, 16);					// bits per sample
	PutTag(wav, "data");
	Put32(wav, dataSize);
	for (size_t i = 0; i < pcm.size(); i++)
		Put16(wav, static_cast<unsigned short>(pcm[i]));
}

int T2S::WriteFile(const std::wstring &fileName, const std::vector<char> &data)
{
	FILE *fp = _wfopen(fileName.c_str(), L"wb");
	if (fp == NULL)
		return 1;
	bool ok = data.empty() || fwrite(&data[0], 1, data.size(), fp) == data.size();
	ok = (fclose(fp) == 0) && ok;
	return ok ? 0 : 2;
}
