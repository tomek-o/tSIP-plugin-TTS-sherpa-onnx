/** \file
	\brief Worker thread: loading models and generating speech without blocking GUI (tSIP main thread)

	Results are posted to a window as message (wParam = 0, lParam = TtsWorker::Result*);
	receiver takes ownership of the Result and deletes it.
*/
#ifndef TTS_WORKER_H
#define TTS_WORKER_H

#include "T2S.h"
#include <windows.h>
#include <string>
#include <vector>

namespace TtsWorker
{

struct Request
{
	enum Type
	{
		LOAD = 0,	///< load model (preload, e.g. after voice is selected)
		SPEAK,		///< generate speech
		RESET		///< destroy all models and unload library (settings changed)
	} type;
	unsigned int id;
	T2S::EngineConfig engine;
	std::string textUtf8;
	int speakerId;
	float speed;
	Request(void): type(SPEAK), id(0), speakerId(0), speed(1.0f) {}
};

struct Result
{
	Request::Type type;
	unsigned int id;
	int status;				///< 0 = OK, 1 = cancelled, other = error
	std::string error;		///< UTF-8
	std::vector<short> pcm;
	int sampleRate;
	unsigned long elapsedMs;
	Result(void): type(Request::SPEAK), id(0), status(0), sampleRate(0), elapsedMs(0) {}
};

/** Start thread; results are posted to hwnd as msg */
int Start(HWND hwnd, UINT msg);
/** Cancel pending work, stop and join thread */
void Stop(void);
void Post(const Request &request);
/** Drop queued SPEAK requests and cancel the one being generated */
void CancelSpeech(void);
/** Number of SPEAK requests queued or being processed */
int GetPendingSpeechCount(void);

}	// namespace TtsWorker

#endif
