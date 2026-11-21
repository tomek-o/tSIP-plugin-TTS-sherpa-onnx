/** \file
	\brief Downloading voices (sherpa-onnx TTS model archives from GitHub releases) and extracting .tar.bz2

	Blocking functions - call from a worker thread.
*/
#ifndef TTS_VOICE_DOWNLOAD_H
#define TTS_VOICE_DOWNLOAD_H

#include <string>
#include <vector>

namespace VoiceDownload
{

struct Asset
{
	std::string name;		///< archive file name, e.g. vits-piper-pl_PL-gosia-medium.tar.bz2
	std::string url;
	unsigned long long size;
	Asset(void): size(0) {}
};

/** Called with number of bytes processed (download: bytes received; extraction: bytes written) */
typedef void (*ProgressFn)(void *arg, unsigned long long done, unsigned long long total);

/** Get list of TTS model archives supported by plugin (vits / kokoro / kitten), sorted by name */
int FetchCatalog(std::vector<Asset> &assets, volatile long *cancel, std::string &error);

int Download(const std::string &url, const std::wstring &fileName, ProgressFn progress, void *arg,
	volatile long *cancel, std::string &error);

/** Extract .tar.bz2 archive
	\param topDir receives name of top level directory from archive (archives contain single directory)
*/
int ExtractTarBz2(const std::wstring &archive, const std::wstring &destDir, std::wstring &topDir,
	ProgressFn progress, void *arg, volatile long *cancel, std::string &error);

/** Delete directory with its contents */
void RemoveDirectoryRecursive(const std::wstring &dir);

}	// namespace VoiceDownload

#endif
