#include "Log.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

namespace
{
	LogSinkFn logSink = NULL;
}

void LogSetSink(LogSinkFn sink)
{
	logSink = sink;
}

void LOG(const char *fmt, ...)
{
	char buf[2048];
	int len = snprintf(buf, sizeof(buf), "TTS-sherpa-onnx: ");
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
	va_end(args);
	buf[sizeof(buf) - 1] = '\0';

	LogSinkFn sink = logSink;
	if (sink)
		sink(buf);
	else
		OutputDebugStringA(buf);
}
