# tSIP text-to-speech plugin (sherpa-onnx, plugin interface v2) - command line build
#
# Windows (MinGW 32-bit, e.g. Code::Blocks 25.03 bundled GCC):
#   mingw32-make                  release build -> bin/Release/TTS_sherpa_onnx.dll
#   mingw32-make CONFIG=debug     debug build   -> bin/Debug/TTS_sherpa_onnx.dll
#   mingw32-make test             command line TTS test (bin/<config>/tts_test.exe)
#   mingw32-make clean
#   or build.bat
#   Release/debug DLL is copied to tSIP Debug_Build\phone and Release_Build\phone (copy_to_tsip.bat).
# Linux (cross compilation, i686-w64-mingw32 toolchain; tests can be run with wine):
#   make, make test, make clean
#
# Keep source list and flags in sync with TTS_sherpa_onnx.cbp (Code::Blocks project).

CONFIG ?= release
NAME = TTS_sherpa_onnx

ifeq ($(OS),Windows_NT)
	# recipes use cmd.exe syntax regardless of sh.exe being in PATH
	SHELL = cmd.exe
	CROSS ?=
	mkdir_p = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
	rm_rf = if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
	COPY_TO_TSIP = 1
else
	CROSS ?= i686-w64-mingw32-
	mkdir_p = mkdir -p $(1)
	rm_rf = rm -rf $(1)
	COPY_TO_TSIP =
endif

CXX = $(CROSS)g++
CC = $(CROSS)gcc
WINDRES = $(CROSS)windres

SOURCES = \
	src/main.cpp \
	src/Plugin.cpp \
	src/SettingsDialog.cpp \
	src/DownloadDialog.cpp \
	src/VoiceDownload.cpp \
	src/Voices.cpp \
	src/QuickTextDialog.cpp \
	src/Settings.cpp \
	src/T2S.cpp \
	src/TtsWorker.cpp \
	src/Utils.cpp \
	src/Log.cpp \
	third_party/jsoncpp/src/lib_json/json_reader.cpp \
	third_party/jsoncpp/src/lib_json/json_value.cpp \
	third_party/jsoncpp/src/lib_json/json_writer.cpp

# bzip2 (decompression of downloaded voice archives)
C_SOURCES = \
	third_party/bzip2/blocksort.c \
	third_party/bzip2/bzlib.c \
	third_party/bzip2/compress.c \
	third_party/bzip2/crctable.c \
	third_party/bzip2/decompress.c \
	third_party/bzip2/huffman.c \
	third_party/bzip2/randtable.c

# plugin interface header: single copy in tSIP source tree (sibling directory)
TSIP_PHONE_DIR = ../tSIP/tSIP/phone

CXXFLAGS = -std=c++17 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 \
	-Ithird_party/jsoncpp/include -Ithird_party/sherpa-onnx -Ithird_party/bzip2 -I$(TSIP_PHONE_DIR)
CFLAGS = -Wall -O2
LDFLAGS = -shared -static -Wl,--add-stdcall-alias
LIBS = -lwinhttp -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lwinmm -lgdi32 -luser32

ifeq ($(CONFIG),debug)
	OUTDIR = bin/Debug
	OBJDIR = obj/Debug
	CXXFLAGS += -g
	CFLAGS += -g
else
	OUTDIR = bin/Release
	OBJDIR = obj/Release
	CXXFLAGS += -Os
	LDFLAGS += -s
endif

TARGET = $(OUTDIR)/$(NAME).dll
OBJECTS = $(addprefix $(OBJDIR)/,$(SOURCES:.cpp=.o)) $(addprefix $(OBJDIR)/,$(C_SOURCES:.c=.o))
RES_OBJ = $(OBJDIR)/resource.o
COPY_STAMP = $(OUTDIR)/copied_to_tsip.stamp

.PHONY: all clean test testhost

ifeq ($(COPY_TO_TSIP),1)
all: $(TARGET) $(COPY_STAMP)
else
all: $(TARGET)
endif

$(TARGET): $(OBJECTS) $(RES_OBJ)
	@$(call mkdir_p,$(OUTDIR))
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(RES_OBJ) $(LIBS)

# marks successful copy to tSIP - copy is retried if it failed (e.g. DLL locked by running tSIP)
$(COPY_STAMP): $(TARGET)
	call copy_to_tsip.bat $(TARGET)
	@echo.> "$(subst /,\,$@)"

$(RES_OBJ): src/resource.rc src/resource.h src/version.h resources/tSIP.ico
	@$(call mkdir_p,$(OBJDIR))
	$(WINDRES) -O coff -Isrc $< -o $@

# bundled third-party jsoncpp: do not report its (harmless) warnings
$(OBJDIR)/third_party/%.o: CXXFLAGS += -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-deprecated-copy

$(OBJDIR)/%.o: %.c
	@$(call mkdir_p,$(dir $@))
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@$(call mkdir_p,$(dir $@))
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# command line test of speech generation (no tSIP needed), see test/tts_test.cpp
test: $(OUTDIR)/tts_test.exe

$(OUTDIR)/tts_test.exe: test/tts_test.cpp src/T2S.cpp src/Utils.cpp src/Log.cpp src/T2S.h
	@$(call mkdir_p,$(OUTDIR))
	$(CXX) -std=c++17 -Wall -Wextra -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 \
		-Ithird_party/sherpa-onnx -Os -static -o $@ test/tts_test.cpp src/T2S.cpp src/Utils.cpp src/Log.cpp

# test of voice catalog / download / extraction, see test/download_test.cpp
test: $(OUTDIR)/download_test.exe

$(OUTDIR)/download_test.exe: test/download_test.cpp src/VoiceDownload.cpp src/Utils.cpp src/Log.cpp $(filter $(OBJDIR)/third_party/%,$(OBJECTS))
	@$(call mkdir_p,$(OUTDIR))
	$(CXX) -std=c++17 -Wall -Wextra -municode -DUNICODE -D_UNICODE -Ithird_party/jsoncpp/include -Ithird_party/bzip2 -Os -static \
		-o $@ test/download_test.cpp src/VoiceDownload.cpp src/Utils.cpp src/Log.cpp $(filter $(OBJDIR)/third_party/%,$(OBJECTS)) -lwinhttp

# test host: loads plugin DLL like tSIP, with fake Invoke() results, drives plugin window (see test/TestHost.cpp)
testhost: $(OUTDIR)/TestHost.exe

$(OUTDIR)/TestHost.exe: test/TestHost.cpp $(TSIP_PHONE_DIR)/tSIP_plugin_v2.h
	@$(call mkdir_p,$(OUTDIR))
	$(CXX) -std=c++17 -Wall -Wextra -municode -DUNICODE -D_UNICODE -I$(TSIP_PHONE_DIR) -Os -static -o $@ test/TestHost.cpp -lgdi32 -luser32

clean:
	-@$(call rm_rf,obj)
	-@$(call rm_rf,bin)

-include $(OBJECTS:.o=.d)
