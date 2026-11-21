/** \file
	\brief tSIP text-to-speech plugin (sherpa-onnx) - plugin interface v2 entry point
*/

#include "Host.h"
#include "Log.h"
#include "Plugin.h"
#include "tSIP_plugin_v2.h"

#include <windows.h>
#include <string.h>
#include <string>

namespace
{

const struct TSIP_HOST_API *host = NULL;

void LogToHost(const char *text)
{
	const struct TSIP_HOST_API *h = host;
	if (h && TSIP_HOST_HAS(h, Log) && h->Log)
		h->Log(h->cookie, text);
}

const char* TSIP_V2_CALL GetMetadata(void)
{
	return "{\"name\":\"TTS (sherpa-onnx)\",\"version\":\"" PLUGIN_VERSION "\","
		"\"description\":\"Text-to-speech: typed text sent as call audio, offline neural voices (sherpa-onnx, Piper)\"}";
}

int TSIP_V2_CALL Start(void)
{
	return Plugin::Start();
}

int TSIP_V2_CALL Stop(void)
{
	Plugin::Stop();
	return 0;
}

void TSIP_V2_CALL Shutdown(void)
{
	Plugin::Stop();
	LogSetSink(NULL);
	host = NULL;
}

void TSIP_V2_CALL OnEvent(const char *type, const char *json)
{
	if (type && json)
		Plugin::OnEvent(type, json);
}

void TSIP_V2_CALL ShowSettings(void *parentHwnd)
{
	Plugin::ShowSettings(static_cast<HWND>(parentHwnd));
}

}	// namespace

int HostInvoke(const char *method, const Json::Value &params, Json::Value &result)
{
	result = Json::Value(Json::objectValue);
	const struct TSIP_HOST_API *h = host;
	if (!h || !TSIP_HOST_HAS(h, Invoke) || !h->Invoke)
		return TSIP_ERR_NOT_AVAILABLE;

	Json::FastWriter writer;
	std::string jsonParams = writer.write(params);
	if (!jsonParams.empty() && jsonParams[jsonParams.size() - 1] == '\n')
		jsonParams.erase(jsonParams.size() - 1);
	char *jsonResult = NULL;
	int status = h->Invoke(h->cookie, method, jsonParams.c_str(), &jsonResult);
	if (jsonResult)
	{
		Json::Reader reader;
		Json::Value parsed;
		if (reader.parse(jsonResult, parsed) && parsed.isObject())
			result = parsed;
		if (TSIP_HOST_HAS(h, Free) && h->Free)
			h->Free(h->cookie, jsonResult);
	}
	return status;
}

const char* HostErrorName(int status)
{
	switch (status)
	{
	case TSIP_OK: return "OK";
	case TSIP_ERR_GENERIC: return "generic error";
	case TSIP_ERR_INVALID_ARG: return "invalid argument";
	case TSIP_ERR_UNKNOWN_METHOD: return "unknown method (tSIP version too old?)";
	case TSIP_ERR_TIMEOUT: return "timeout";
	case TSIP_ERR_NOT_AVAILABLE: return "not available";
	default: return "unknown error";
	}
}

extern "C" __declspec(dllexport) int TSIP_V2_CALL tSipPluginInit(const struct TSIP_HOST_API *hostApi, struct TSIP_PLUGIN_API *plugin)
{
	if (hostApi == NULL || plugin == NULL)
		return -1;
	if (!TSIP_HOST_HAS(hostApi, Log) || !TSIP_HOST_HAS(hostApi, Invoke) || !TSIP_HOST_HAS(hostApi, Free))
		return -2;
	host = hostApi;
	LogSetSink(LogToHost);

	if (TSIP_PLUGIN_HAS(plugin, GetMetadata))
		plugin->GetMetadata = GetMetadata;
	if (TSIP_PLUGIN_HAS(plugin, Start))
		plugin->Start = Start;
	if (TSIP_PLUGIN_HAS(plugin, Stop))
		plugin->Stop = Stop;
	if (TSIP_PLUGIN_HAS(plugin, Shutdown))
		plugin->Shutdown = Shutdown;
	if (TSIP_PLUGIN_HAS(plugin, OnEvent))
		plugin->OnEvent = OnEvent;
	if (TSIP_PLUGIN_HAS(plugin, ShowSettings))
		plugin->ShowSettings = ShowSettings;
	return 0;
}
