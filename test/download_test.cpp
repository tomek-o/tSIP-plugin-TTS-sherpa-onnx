/** \file
	\brief Test of voice catalog / download / extraction (VoiceDownload) without tSIP

	Usage: download_test.exe <destDir> [archive name, e.g. vits-piper-pl_PL-gosia-medium-int8.tar.bz2] [local .tar.bz2 instead of download]
*/
#include "../src/VoiceDownload.h"
#include "../src/Utils.h"
#include "../src/Log.h"
#include <windows.h>
#include <stdio.h>
#include <string>

namespace
{

void LogToStdout(const char *text)
{
	fputs(text, stdout);
}

void Progress(void *, unsigned long long done, unsigned long long total)
{
	static DWORD last = 0;
	if (GetTickCount() - last > 1000)
	{
		last = GetTickCount();
		printf("  %.1f / %.1f MB\n", done / 1048576.0, total / 1048576.0);
	}
}

}	// namespace

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage: download_test <destDir> [archive name] [local archive]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	LogSetSink(LogToStdout);
	std::wstring dest = argv[1];
	std::string error;
	volatile long cancel = 0;
	std::wstring archive;

	if (argc > 3)
	{
		archive = argv[3];
	}
	else
	{
		std::vector<VoiceDownload::Asset> assets;
		DWORD start = GetTickCount();
		if (VoiceDownload::FetchCatalog(assets, &cancel, error) != 0)
		{
			printf("FetchCatalog failed: %s\n", error.c_str());
			return 1;
		}
		printf("Catalog: %u archives in %lu ms\n", static_cast<unsigned int>(assets.size()), GetTickCount() - start);
		if (argc < 3)
			return 0;
		std::string wanted = Utils::WideToUtf8(argv[2]);
		const VoiceDownload::Asset *asset = NULL;
		for (unsigned int i = 0; i < assets.size(); i++)
		{
			if (assets[i].name == wanted)
				asset = &assets[i];
		}
		if (!asset)
		{
			printf("Archive %s not found in catalog\n", wanted.c_str());
			return 1;
		}
		archive = dest + L"\\" + argv[2] + L".part";
		start = GetTickCount();
		if (VoiceDownload::Download(asset->url, archive, Progress, NULL, &cancel, error) != 0)
		{
			printf("Download failed: %s\n", error.c_str());
			return 1;
		}
		printf("Downloaded in %lu ms\n", GetTickCount() - start);
	}

	std::wstring topDir;
	DWORD start = GetTickCount();
	if (VoiceDownload::ExtractTarBz2(archive, dest, topDir, Progress, NULL, &cancel, error) != 0)
	{
		printf("Extraction failed: %s\n", error.c_str());
		return 1;
	}
	printf("Extracted to %s in %lu ms\n", Utils::WideToUtf8(dest + L"\\" + topDir).c_str(), GetTickCount() - start);
	return 0;
}
