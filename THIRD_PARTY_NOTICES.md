# Third party components

| Component | Where | License | Source |
|-----------|-------|---------|--------|
| sherpa-onnx 1.13.8 | `sherpa-onnx-c-api.dll` (prebuilt, 32-bit, `win-x86-shared-MT-Release`); `third_party/sherpa-onnx/c-api.h` | Apache-2.0 | https://github.com/k2-fsa/sherpa-onnx/tree/v1.13.8 |
| espeak-ng (phonemization, inside `sherpa-onnx-c-api.dll`) | `sherpa-onnx-c-api.dll`, `espeak-ng-data` directory of each voice | GPL-3.0-or-later | https://github.com/espeak-ng/espeak-ng (version used by sherpa-onnx 1.13.8, see its CMake files) |
| ONNX Runtime 1.28.2 | `onnxruntime.dll` (prebuilt, distributed with sherpa-onnx) | MIT; components included in it: see `onnxruntime_ThirdPartyNotices.txt` | https://github.com/microsoft/onnxruntime |
| bzip2 1.0.8 | `third_party/bzip2` (compiled into plugin DLL) | bzip2 license (BSD-style) | https://sourceware.org/bzip2/ |
| jsoncpp (old version, as used by tSIP) | `third_party/jsoncpp` (compiled into plugin DLL) | public domain / MIT | https://github.com/open-source-parsers/jsoncpp |

License texts for redistributed binaries are in `_dist/phone/TTS_sherpa_onnx/licenses`.
`onnxruntime_ThirdPartyNotices.txt` (third party components compiled into onnxruntime: protobuf, abseil,
Eigen, flatbuffers, ...) is taken unmodified from the official onnxruntime 1.28.0 Python package
(`onnxruntime/ThirdPartyNotices.txt` in `onnxruntime-1.28.0-*.whl`, PyPI) - 1.28.2 is not published there,
patch releases do not change the set of components.

Note on GPL: `sherpa-onnx-c-api.dll` contains espeak-ng (GPL-3.0-or-later), so this DLL is
distributed under GPL-3.0 terms (license text included; corresponding source: sherpa-onnx v1.13.8
and espeak-ng sources from the repositories above). The plugin DLL loads it into the same process
and calls its API - a package containing both should be treated as a combined work under
GPL-3.0. The plugin source code itself is BSD 3-Clause, which is GPL-compatible, and is
published together with the binaries, so the combination can be distributed under GPL-3.0
terms. (Not legal advice.)

## Voices

Voices are not part of the plugin; they are downloaded by the user (Settings / Download voices)
or copied manually. Each voice has its own license - see `MODEL_CARD` in the voice directory.
Examples:

| Voice | Dataset license |
|-------|-----------------|
| pl_PL gosia (Piper) | CC0 |
| en_US lessac (Piper) | Blizzard 2013 Lessac dataset license - check terms before use, especially commercial: https://www.cstr.ed.ac.uk/projects/blizzard/2013/lessac_blizzard2013/license.html |
