/** \file
	\brief Command line test of T2S module (sherpa-onnx engine) without tSIP

	Usage: tts_test.exe <libraryDir> <modelType> <model> <tokens> <dataDir> <voices|-> <text file (UTF-8)> <output.wav> [speakerId] [speed]
*/
#include "../src/T2S.h"
#include "../src/Utils.h"
#include "../src/Log.h"
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

namespace
{

void LogToStdout(const char *text)
{
	fputs(text, stdout);
	fflush(stdout);
}

bool ReadUtf8File(const std::wstring &name, std::string &text)
{
	FILE *fp = _wfopen(name.c_str(), L"rb");
	if (!fp)
		return false;
	char buf[4096];
	size_t len;
	while ((len = fread(buf, 1, sizeof(buf), fp)) > 0)
		text.append(buf, len);
	fclose(fp);
	if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
		text.erase(0, 3);
	return true;
}

}	// namespace

int wmain(int argc, wchar_t **argv)
{
	if (argc < 9)
	{
		fprintf(stderr, "Usage: tts_test <libraryDir> <modelType> <model> <tokens> <dataDir> <voices|-> <text.txt> <out.wav> [speakerId] [speed]\n");
		return 1;
	}
	_controlfp(_MCW_EM, _MCW_EM);	// as in plugin worker thread
	LogSetSink(LogToStdout);

	T2S::EngineConfig cfg;
	cfg.libraryDir = argv[1];
	cfg.modelType = Utils::WideToUtf8(argv[2]);
	cfg.model = argv[3];
	cfg.tokens = argv[4];
	cfg.dataDir = argv[5];
	if (std::wstring(argv[6]) != L"-")
		cfg.voices = argv[6];
	cfg.numThreads = 2;
	int sid = argc > 9 ? _wtoi(argv[9]) : 0;
	float speed = argc > 10 ? (float)_wtof(argv[10]) : 1.0f;

	std::string text;
	if (!ReadUtf8File(argv[7], text))
	{
		fprintf(stderr, "Cannot read text file\n");
		return 1;
	}

	std::string error;
	if (T2S::LoadSherpaLibrary(cfg.libraryDir, error) != 0)
	{
		fprintf(stderr, "LoadSherpaLibrary: %s\n", error.c_str());
		return 2;
	}
	{
		T2S::Engine engine;
		if (engine.Create(cfg, error) != 0)
		{
			fprintf(stderr, "Create: %s\n", error.c_str());
			return 3;
		}
		std::vector<short> pcm;
		volatile long cancel = 0;
		DWORD start = GetTickCount();
		if (engine.Generate(text, sid, speed, &cancel, pcm, error) != 0)
		{
			fprintf(stderr, "Generate: %s\n", error.c_str());
			return 4;
		}
		DWORD elapsed = GetTickCount() - start;
		double audioSec = (double)pcm.size() / engine.GetSampleRate();
		printf("Generated %u samples (%.2f s audio) at %d Hz in %lu ms (RTF %.3f)\n", (unsigned)pcm.size(),
			audioSec, engine.GetSampleRate(), elapsed, audioSec > 0 ? elapsed / 1000.0 / audioSec : 0.0);
		std::vector<char> wav;
		T2S::MakeWav(pcm, engine.GetSampleRate(), wav);
		if (T2S::WriteFile(argv[8], wav) != 0)
		{
			fprintf(stderr, "Cannot write output file\n");
			return 5;
		}
	}
	T2S::UnloadSherpaLibrary();
	return 0;
}
