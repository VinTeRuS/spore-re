# Native host (no Wine): the recompiled engine plus the Win32 emulation layer.
#   make                         Linux x86-64 executable (build/native/proto-native)
#   make PLATFORM=android [ABI=arm64-v8a|x86_64]
#                                libmain.so for the APK (android/build-apk.sh), against the libraries from
#                                android/build-deps.sh
PLATFORM ?= linux
# Generated at the exe's own base: third_party/x86recomp/tools/recomp.py --functions all --split 400 --out build/recomp/all
RECOMP_DIR ?= ../../build/recomp/all
RUNTIME := ../../third_party/x86recomp/runtime
DXVK ?= $(HOME)/spore-re-data/deps/dxvk
# Guest memory: a 4 GiB window at 64 GiB (guest.h kGuestBase), clear of the low 4 GiB that Android's Java heap and
# Qualcomm's GPU driver use.
COMMON_FLAGS := -DRECOMP_FLAT_MEMORY -DRECOMP_GUEST_BASE=0x1000000000ull -fsigned-char -I$(RUNTIME)
# Generated code: flat memory turns G() into plain pointers. -O1 on the desktop keeps rebuilds quick; Android gets
# -O2 (clang: barely slower to build).
RECOMP_OPT ?= -O1
RECOMP_CFLAGS = -std=c11 $(RECOMP_OPT) $(RECOMP_EXTRA) -g0 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -w $(COMMON_FLAGS) \
                $(TARGET_FLAGS)
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter $(COMMON_FLAGS) $(TARGET_FLAGS) \
           -I$(DXVK)/include/native/directx -I$(DXVK)/include/native/windows -I$(DXVK)/include/native/wsi \
           $(DEP_INCLUDES)

ifeq ($(PLATFORM),android)
ABI ?= arm64-v8a
NDK ?= $(HOME)/Android/Sdk/ndk/28.2.13676358
DEPS ?= $(HOME)/spore-re-data/android-deps/$(ABI)
TRIPLE := $(if $(filter x86_64,$(ABI)),x86_64,aarch64)-linux-android26
TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX := $(TOOLCHAIN)/$(TRIPLE)-clang++
CC := $(TOOLCHAIN)/$(TRIPLE)-clang
OUT ?= ../../build/android/$(ABI)
# arm64: ARMv8.2 + RCpc (Cortex-A55/A76/X1 and newer, i.e. phones from 2019 on): LDAPR for the TSO acquire loads,
# inline LSE atomics. ARM_ARCH=armv8-a builds for older cores (Cortex-A53/A73), with outline atomics.
ARM_ARCH ?= armv8.2-a+rcpc
TARGET_FLAGS := -fPIC $(if $(filter arm64-v8a,$(ABI)),-march=$(ARM_ARCH) -mtune=cortex-a76 \
                $(if $(filter armv8-a,$(ARM_ARCH)),-moutline-atomics))
RECOMP_OPT := -O2
# Profile-guided optimization: PGO=gen builds an instrumented libmain.so that writes spore-pgo-*.profraw into the
# app's files folder every minute (main.cpp); PGO=<merged .profdata> builds with that profile (android/pgo.sh).
PGO ?=
ifeq ($(PGO),gen)
PGO_FLAGS := -fprofile-generate -DSPORERE_PGO_GEN
PGO_LDFLAGS := -fprofile-generate
else ifneq ($(PGO),)
PGO_FLAGS := -fprofile-use=$(PGO) -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
PGO_LDFLAGS :=
endif
# arm64 is weakly ordered: guest memory accesses get x86-like ordering (runtime.h RECOMP_TSO); tso=0 to measure.
TSO ?= 1
RECOMP_EXTRA := $(if $(filter 1,$(TSO)),-DRECOMP_TSO)
DEP_INCLUDES := -I$(DEPS)/include
LDFLAGS := -shared -Wl,-Bsymbolic -static-libstdc++ -Wl,--no-undefined -L$(DEPS)/lib -ldxvk_d3d9 -lSDL2 -llog -landroid
TARGET_FLAGS += $(PGO_FLAGS) $(if $(filter 1,$(SPORERE_VK11)),-DSPORERE_VK11)
LDFLAGS += $(PGO_LDFLAGS)
# Adreno: Turnip and libadrenotools from android/build-turnip.sh (arm64 only; built without them otherwise).
TURNIP ?= $(HOME)/spore-re-data/android-turnip
ifneq ($(and $(filter arm64-v8a,$(ABI)),$(wildcard $(TURNIP)/lib/libadrenotools.a)),)
DEP_INCLUDES += -I$(TURNIP)/include -DSPORERE_ADRENOTOOLS
LDFLAGS += -L$(TURNIP)/lib -ladrenotools -llinkernsbypass -ldl \
           -Wl,--defsym=android_get_device_api_level=sporere_device_api_level
# The driver probe program (turnip_probe.cpp): API 29 like libadrenotools, plain ARMv8.0 so one build runs on all.
EXTRA := $(OUT)/libturnipprobe.so
endif
BINARY := libmain.so
else
CXX ?= g++
CC ?= gcc
OUT ?= ../../build/native
# DXVK native (D3D9 on Vulkan) for the desktop build.
DXVK_LIB ?= $(DXVK)/build-native/src/d3d9
TARGET_FLAGS :=
DEP_INCLUDES :=
LDFLAGS := -pthread -L$(DXVK_LIB) -Wl,-rpath,$(DXVK_LIB) -ldxvk_d3d9 -lSDL2
BINARY := spore-native
endif

SRCS := main.cpp memory.cpp loader.cpp imports.cpp runtime.cpp thread.cpp kernel32.cpp msvcrt.cpp user32.cpp \
        advapi32.cpp misc.cpp d3d9.cpp input.cpp dinput.cpp gamepad.cpp config.cpp watchdog.cpp vulkan_driver.cpp \
        perf_hint.cpp stubs.cpp dsound.cpp profiler.cpp
GEN := $(wildcard $(RECOMP_DIR)/*.c)
OBJS = $(SRCS:%.cpp=$(OUT)/%.o) $(GEN:$(RECOMP_DIR)/%.c=$(OUT)/gen/%.o)

all: $(OUT)/$(BINARY) $(EXTRA)

$(OUT)/libturnipprobe.so: turnip_probe.cpp turnip_load.h
	@mkdir -p $(dir $@)
	$(TOOLCHAIN)/aarch64-linux-android29-clang++ -std=c++17 -O2 -fPIE -pie -march=armv8-a -moutline-atomics -static-libstdc++ \
		-I$(TURNIP)/include -o $@ $< -L$(TURNIP)/lib -ladrenotools -llinkernsbypass -ldl -llog

$(OUT)/$(BINARY): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: %.cpp guest.h win.h turnip_load.h $(RUNTIME)/runtime.h d3d9_gen.inc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/gen/%.o: $(RECOMP_DIR)/%.c $(RUNTIME)/runtime.h $(RUNTIME)/runtime_fp.h $(RUNTIME)/runtime_simd.h
	@mkdir -p $(dir $@)
	$(CC) $(RECOMP_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
