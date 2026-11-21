/** \file
	\brief Text-to-speech using sherpa-onnx C API (sherpa-onnx-c-api.dll loaded at runtime)

	Struct layout and function signatures come from third_party/sherpa-onnx/c-api.h,
	matching sherpa-onnx v1.13.8 - the DLL must be the same version (structures passed
	by pointer change between versions; v2.x dropped built-in espeak-ng phonemization
	required by Piper voices).

	Not thread-safe: use from single (worker) thread.
*/
#ifndef TTS_T2S_H
#define TTS_T2S_H

#include <string>
#include <vector>

namespace T2S
{

/** Version of sherpa-onnx the plugin is built for */
extern const char* const SHERPA_ONNX_VERSION;

/** Full paths (already resolved), UTF-16 */
struct EngineConfig
{
	std::wstring libraryDir;
	int numThreads;
	std::string modelType;	///< "vits", "kokoro", "kitten"
	std::wstring model;
	std::wstring tokens;
	std::wstring dataDir;
	std::wstring lexicon;
	std::wstring voices;
	std::string lang;

	EngineConfig(void): numThreads(2) {}
	/** Text identifying model + options requiring reload */
	std::wstring Key(void) const;
};

/** Load sherpa-onnx-c-api.dll (with onnxruntime.dll from the same directory) */
int LoadSherpaLibrary(const std::wstring &dir, std::string &error);
void UnloadSherpaLibrary(void);
bool IsLibraryLoaded(void);
/** Version reported by loaded DLL */
std::string GetLibraryVersion(void);

class Engine
{
public:
	Engine(void);
	~Engine(void);
	/** Create TTS from model files; library must be loaded */
	int Create(const EngineConfig &cfg, std::string &error);
	void Destroy(void);
	bool IsCreated(void) const { return tts != NULL; }
	int GetSampleRate(void) const { return sampleRate; }
	int GetNumSpeakers(void) const { return numSpeakers; }

	/** Generate speech
		\param textUtf8 text to speak
		\param cancel if not NULL: generation is stopped when *cancel becomes non-zero (checked after each sentence)
		\param pcm output, 16-bit mono
		\return 0 on success, 1 if cancelled, other value on error
	*/
	int Generate(const std::string &textUtf8, int speakerId, float speed, volatile long *cancel,
		std::vector<short> &pcm, std::string &error);
private:
	Engine(const Engine&);
	Engine& operator=(const Engine&);
	const void *tts;
	int sampleRate;
	int numSpeakers;
};

/** Build WAV (RIFF, PCM 16-bit mono) file image */
void MakeWav(const std::vector<short> &pcm, int sampleRate, std::vector<char> &wav);
int WriteFile(const std::wstring &fileName, const std::vector<char> &data);

}	// namespace T2S

#endif
