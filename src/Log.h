#ifndef TTS_LOG_H
#define TTS_LOG_H

typedef void (*LogSinkFn)(const char *text);

/** Set function receiving formatted log lines (e.g. tSIP log callback wrapper or stdout in test app).
	The sink must be thread-safe: the TTS worker thread logs too.
*/
void LogSetSink(LogSinkFn sink);

void LOG(const char *fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 1, 2)))
#endif
	;

#endif
