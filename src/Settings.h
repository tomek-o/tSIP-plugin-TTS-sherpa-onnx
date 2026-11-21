#ifndef TTS_SETTINGS_H
#define TTS_SETTINGS_H

#include <string>
#include <vector>

/** All strings are UTF-8. Relative paths are resolved against the plugin data directory
	(<plugin DLL directory>\<plugin DLL name>, e.g. phone\TTS_sherpa_onnx).
*/
struct VoiceConfig
{
	std::string name;		///< displayed in main window voice list
	std::string modelType;	///< "vits" (Piper, MMS, ...), "kokoro" or "kitten"
	std::string model;		///< .onnx model file
	std::string tokens;		///< tokens.txt
	std::string dataDir;	///< espeak-ng-data directory
	std::string lexicon;	///< optional
	std::string voices;		///< voices.bin (kokoro / kitten only)
	std::string lang;		///< optional language hint (kokoro only)
	int speakerId;			///< for multi-speaker models
	double speed;			///< 1.0 = normal, > 1 = faster

	VoiceConfig(void);
	/** Compare fields that require reloading the model */
	bool SameModel(const VoiceConfig &other) const;
};

/** Predefined text sent with quick text button */
struct QuickText
{
	std::string caption;	///< button caption; empty: beginning of the text
	std::string text;
	std::string voice;		///< voice name; empty: current voice
};

class Settings
{
public:
	Settings(void);
	void SetDefault(void);
	int Read(const std::wstring &fileName);
	int Write(const std::wstring &fileName) const;

	struct Window
	{
		int x, y, width, height;
		bool maximized;
		bool alwaysOnTop;
	} wnd;

	bool sendOnEnter;
	bool showVoiceInHistory;	///< prefix messages in history with voice name

	std::string libraryDir;	///< directory with sherpa-onnx-c-api.dll + onnxruntime.dll; empty = plugin data directory
	int numThreads;

	std::vector<VoiceConfig> voices;
	int currentVoice;

	/** Replace call audio input (microphone) with silence when call is established and between messages;
		false: microphone is restored after each message */
	bool silenceMicrophone;

	bool sidetone;			///< hear transmitted audio (TTS) locally during call
	int sidetoneLevel;		///< percent, 0..200

	/** tSIP programmable button (ID) used as container for plugin window (window embedded in button);
		0: separate window */
	int embedButtonId;

	enum { QUICK_TEXT_COUNT = 6 };
	std::vector<QuickText> quickTexts;	///< always QUICK_TEXT_COUNT elements
	bool showQuickTexts;
	bool quickTextClickSends;	///< true: click speaks text, false: click inserts text into input field

	const VoiceConfig* GetCurrentVoice(void) const;
};

extern Settings appSettings;

#endif
