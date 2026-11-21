/** \file
	\brief Access to tSIP host functions (plugin interface v2), implemented in main.cpp
*/
#ifndef TTS_HOST_H
#define TTS_HOST_H

#include <json/json.h>

/** \brief Call host Invoke() - from main thread only in this plugin
	\param result parsed JSON result (object), may contain "error" also on failure
	\return TSIP_OK or TSIP_ERR_* code
*/
int HostInvoke(const char *method, const Json::Value &params, Json::Value &result);

/** \brief Human readable name of TSIP_ERR_* code */
const char* HostErrorName(int status);

#endif
