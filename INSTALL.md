# Installing tSIP text-to-speech plugin (sherpa-onnx)

Requirements:
- tSIP with plugin interface v2 (version 0.4+)
- Windows 8.1 or newer (tested only on Windows 10 and Wine at the moment)
- 20 MB for the plugin and libraries, 60-110 MB per voice

## 1. Copy files

Copy into tSIP \phone directory (directory for plugins):

- TTS_sherpa_onnx.dll - built plugin
- contents of _dist\phone from this repository (sherpa-onnx libraries, license texts).

Resulting layout:

    tSIP.exe
    phone\TTS_sherpa_onnx.dll
    phone\TTS_sherpa_onnx\sherpa-onnx-c-api.dll
    phone\TTS_sherpa_onnx\onnxruntime.dll
    phone\TTS_sherpa_onnx\licenses\...
    phone\TTS_sherpa_onnx\vits-piper-pl_PL-gosia-medium\...      (voices, step 3)

## 2. Enable plugin

tSIP: Settings / "Plugins / phones" -> enable TTS_sherpa_onnx.dll, apply settings.

Plugin window ("tSIP text-to-speech") would appear. Instead of separate window you can embed TTS window in selected button
(choose something like button 55 or 70, reposition it and increase its size, select button ID in plugin configuration).

## 3. Install voices

Download voices using built-in download function or from 
https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/.
