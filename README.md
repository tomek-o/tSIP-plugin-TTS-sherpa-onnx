# tSIP text-to-speech plugin (sherpa-onnx)

Text-to-speech plugin with chat-style interface.

Similar to previous tSIP-plugin-TTS, but using sherpa-onnx engine and downloaded voices
instead of Microsoft SAPI.

Requires tSIP 0.4+ and Windows 8.1+ or Wine.

![TTS](http://tomeko.net/software/SIPclient/TTS_sherpa_onnx/tts_on_the_button.png) 

![TTS_settings](http://tomeko.net/software/SIPclient/TTS_sherpa_onnx/tts_settings.png) 

## Installation

See [INSTALL.md](INSTALL.md) - in short: copy `TTS_sherpa_onnx.dll` and `_dist\phone` contents to tSIP
`phone` directory, enable plugin in tSIP Settings / "Plugins / phones", install voices from plugin
Settings / "Download voices...". Requires Windows 8.1 or newer.

## Usage

- type text, Enter (or Send button) - Shift+Enter: new line (if "[Enter] to send" is checked)
- voice selection: combo box in window
- `...` button: Settings, Clear log, Always on top, Show voice name in log, Show quick text buttons, About
- quick text buttons (row above input field):
  - click: speaks the text (or inserts it into input field if "Click sends immediately" is unchecked);
    Ctrl+1 ... Ctrl+6 in input field do the same
  - empty button (`+`): click to define text
  - right click: Send / Insert into input field / Edit... / Clear, "Click sends immediately",
    "Show quick text buttons"
  - Edit: caption (short label; empty: beginning of the text), text, voice (empty: current voice -
    e.g. English greeting while Polish voice is selected); full text is shown as tooltip
- message is moved to the log when it is played completely
- no call: message is played locally (voice preview)

## License

BSD 3-Clause, third party components: THIRD_PARTY_NOTICES.md.
