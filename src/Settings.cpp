#include "Settings.h"
#include "Utils.h"
#include "Log.h"
#include <json/json.h>
#include <stdio.h>
#include <windows.h>

Settings appSettings;

namespace
{

bool ReadTextFile(const std::wstring &fileName, std::string &content)
{
	FILE *fp = _wfopen(fileName.c_str(), L"rb");
	if (fp == NULL)
		return false;
	char buf[4096];
	size_t len;
	while ((len = fread(buf, 1, sizeof(buf), fp)) > 0)
		content.append(buf, len);
	fclose(fp);
	return true;
}

std::string GetString(const Json::Value &jv, const char *name, const std::string &def)
{
	const Json::Value &v = jv[name];
	if (v.isString())
		return v.asString();
	return def;
}

int GetInt(const Json::Value &jv, const char *name, int def)
{
	const Json::Value &v = jv[name];
	if (v.isInt() || v.isUInt())
		return v.asInt();
	return def;
}

double GetDouble(const Json::Value &jv, const char *name, double def)
{
	const Json::Value &v = jv[name];
	if (v.isNumeric())
		return v.asDouble();
	return def;
}

bool GetBool(const Json::Value &jv, const char *name, bool def)
{
	const Json::Value &v = jv[name];
	if (v.isBool())
		return v.asBool();
	return def;
}

}	// namespace

VoiceConfig::VoiceConfig(void):
	modelType("vits"),
	speakerId(0),
	speed(1.0)
{
}

bool VoiceConfig::SameModel(const VoiceConfig &other) const
{
	return modelType == other.modelType &&
		model == other.model &&
		tokens == other.tokens &&
		dataDir == other.dataDir &&
		lexicon == other.lexicon &&
		voices == other.voices &&
		lang == other.lang;
}

Settings::Settings(void)
{
	SetDefault();
}

void Settings::SetDefault(void)
{
	wnd.x = 30;
	wnd.y = 30;
	wnd.width = 420;
	wnd.height = 360;
	wnd.maximized = false;
	wnd.alwaysOnTop = false;

	sendOnEnter = true;
	showVoiceInHistory = true;
	libraryDir = "";
	numThreads = 2;

	voices.clear();	// installed voices are added on first start (Voices::FindInstalled)
	currentVoice = 0;

	silenceMicrophone = true;
	quickTexts.assign(QUICK_TEXT_COUNT, QuickText());
	showQuickTexts = true;
	quickTextClickSends = true;
	sidetone = false;
	sidetoneLevel = 50;
	embedButtonId = 0;
}

const VoiceConfig* Settings::GetCurrentVoice(void) const
{
	if (currentVoice < 0 || currentVoice >= (int)voices.size())
		return NULL;
	return &voices[currentVoice];
}

int Settings::Read(const std::wstring &fileName)
{
	SetDefault();

	std::string content;
	if (!ReadTextFile(fileName, content))
		return 1;

	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(content, root) || !root.isObject())
	{
		LOG("Failed to parse settings file\n");
		return 2;
	}

	try
	{
		const Json::Value &jw = root["Window"];
		if (jw.isObject())
		{
			embedButtonId = GetInt(jw, "EmbedButtonId", embedButtonId);
			if (embedButtonId < 0)
				embedButtonId = 0;
			wnd.x = GetInt(jw, "X", wnd.x);
			wnd.y = GetInt(jw, "Y", wnd.y);
			wnd.width = GetInt(jw, "Width", wnd.width);
			wnd.height = GetInt(jw, "Height", wnd.height);
			wnd.maximized = GetBool(jw, "Maximized", wnd.maximized);
			wnd.alwaysOnTop = GetBool(jw, "AlwaysOnTop", wnd.alwaysOnTop);
		}

		const Json::Value &jt = root["Tts"];
		if (jt.isObject())
		{
			sendOnEnter = GetBool(jt, "SendOnEnter", sendOnEnter);
			showVoiceInHistory = GetBool(jt, "ShowVoiceInHistory", showVoiceInHistory);
			silenceMicrophone = GetBool(jt, "SilenceMicrophone", silenceMicrophone);
			sidetone = GetBool(jt, "Sidetone", sidetone);
			sidetoneLevel = GetInt(jt, "SidetoneLevel", sidetoneLevel);
			if (sidetoneLevel < 0 || sidetoneLevel > 200)
				sidetoneLevel = 50;
			libraryDir = GetString(jt, "LibraryDir", libraryDir);
			numThreads = GetInt(jt, "NumThreads", numThreads);
			if (numThreads < 1 || numThreads > 16)
				numThreads = 2;

			const Json::Value &jvs = jt["Voices"];
			if (jvs.isArray())
			{
				voices.clear();
				for (unsigned int i = 0; i < jvs.size(); i++)
				{
					const Json::Value &jv = jvs[i];
					if (!jv.isObject())
						continue;
					VoiceConfig v;
					v.name = GetString(jv, "Name", v.name);
					v.modelType = GetString(jv, "ModelType", v.modelType);
					v.model = GetString(jv, "Model", v.model);
					v.tokens = GetString(jv, "Tokens", v.tokens);
					v.dataDir = GetString(jv, "DataDir", v.dataDir);
					v.lexicon = GetString(jv, "Lexicon", v.lexicon);
					v.voices = GetString(jv, "VoicesFile", v.voices);
					v.lang = GetString(jv, "Lang", v.lang);
					v.speakerId = GetInt(jv, "SpeakerId", v.speakerId);
					v.speed = GetDouble(jv, "Speed", v.speed);
					if (v.speed < 0.25 || v.speed > 4.0)
						v.speed = 1.0;
					voices.push_back(v);
				}
			}
			currentVoice = GetInt(jt, "CurrentVoice", currentVoice);

			showQuickTexts = GetBool(jt, "ShowQuickTexts", showQuickTexts);
			quickTextClickSends = GetBool(jt, "QuickTextClickSends", quickTextClickSends);
			const Json::Value &jq = jt["QuickTexts"];
			if (jq.isArray())
			{
				for (unsigned int i = 0; i < jq.size() && i < quickTexts.size(); i++)
				{
					const Json::Value &q = jq[i];
					if (!q.isObject())
						continue;
					quickTexts[i].caption = GetString(q, "Caption", "");
					quickTexts[i].text = GetString(q, "Text", "");
					quickTexts[i].voice = GetString(q, "Voice", "");
				}
			}
			if (currentVoice < 0 || currentVoice >= (int)voices.size())
				currentVoice = 0;
		}
	}
	catch (...)
	{
		LOG("Unexpected settings file structure\n");
		return 3;
	}

	return 0;
}

int Settings::Write(const std::wstring &fileName) const
{
	Json::Value root;

	Json::Value &jw = root["Window"];
	jw["X"] = wnd.x;
	jw["Y"] = wnd.y;
	jw["Width"] = wnd.width;
	jw["Height"] = wnd.height;
	jw["Maximized"] = wnd.maximized;
	jw["AlwaysOnTop"] = wnd.alwaysOnTop;
	jw["EmbedButtonId"] = embedButtonId;

	Json::Value &jt = root["Tts"];
	jt["SendOnEnter"] = sendOnEnter;
	jt["ShowVoiceInHistory"] = showVoiceInHistory;
	jt["SilenceMicrophone"] = silenceMicrophone;
	jt["Sidetone"] = sidetone;
	jt["SidetoneLevel"] = sidetoneLevel;
	jt["LibraryDir"] = libraryDir;
	jt["NumThreads"] = numThreads;
	jt["CurrentVoice"] = currentVoice;
	jt["ShowQuickTexts"] = showQuickTexts;
	jt["QuickTextClickSends"] = quickTextClickSends;
	Json::Value &jq = jt["QuickTexts"];
	jq = Json::Value(Json::arrayValue);
	for (unsigned int i = 0; i < quickTexts.size(); i++)
	{
		Json::Value q;
		q["Caption"] = quickTexts[i].caption;
		q["Text"] = quickTexts[i].text;
		q["Voice"] = quickTexts[i].voice;
		jq.append(q);
	}
	Json::Value &jvs = jt["Voices"];
	jvs = Json::Value(Json::arrayValue);
	for (unsigned int i = 0; i < voices.size(); i++)
	{
		const VoiceConfig &v = voices[i];
		Json::Value jv;
		jv["Name"] = v.name;
		jv["ModelType"] = v.modelType;
		jv["Model"] = v.model;
		jv["Tokens"] = v.tokens;
		jv["DataDir"] = v.dataDir;
		jv["Lexicon"] = v.lexicon;
		jv["VoicesFile"] = v.voices;
		jv["Lang"] = v.lang;
		jv["SpeakerId"] = v.speakerId;
		jv["Speed"] = v.speed;
		jvs.append(jv);
	}

	Json::StyledWriter writer;
	std::string output = writer.write(root);

	// write to temporary file first, then replace - settings are not lost if writing is interrupted
	std::wstring tmpName = fileName + L".tmp";
	FILE *fp = _wfopen(tmpName.c_str(), L"wb");
	if (fp == NULL)
	{
		LOG("Failed to create settings file\n");
		return 1;
	}
	bool ok = fwrite(output.data(), 1, output.size(), fp) == output.size();
	ok = (fflush(fp) == 0) && ok;
	ok = (fclose(fp) == 0) && ok;
	if (!ok)
	{
		LOG("Failed to write settings file\n");
		DeleteFileW(tmpName.c_str());
		return 1;
	}
	if (!MoveFileExW(tmpName.c_str(), fileName.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		LOG("Failed to replace settings file, error %lu\n", GetLastError());
		DeleteFileW(tmpName.c_str());
		return 1;
	}
	return 0;
}
