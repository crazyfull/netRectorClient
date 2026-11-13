# Two shared libraries are built:
#   libhev-socks5-tunnel.so  tun2socks engine (git submodule third_party/hev-socks5-tunnel)
#   libmuxtunnel.so          netRectorClient + mux-tunnelClient core + JNI bridge
MY_PATH := $(call my-dir)
REPO_ROOT := $(abspath $(MY_PATH)/../../../../../..)
HEV_DIR := $(REPO_ROOT)/example/android-mux-tunnel/third_party/hev-socks5-tunnel

ifeq ($(wildcard $(HEV_DIR)/Android.mk),)
$(error hev-socks5-tunnel is missing: run "git submodule update --init --recursive")
endif

# hev-socks5-tunnel (also defines its own yaml, lwip and hev-task-system modules)
include $(HEV_DIR)/Android.mk

LOCAL_PATH := $(MY_PATH)
include $(CLEAR_VARS)
LOCAL_MODULE := muxtunnel

CLIENT_SRC_DIR := $(REPO_ROOT)/src
EXAMPLE_SRC_DIR := $(REPO_ROOT)/example/mux-tunnelClient/src

LOCAL_SRC_FILES := $(MY_PATH)/muxtunnel_jni.cpp \
    $(wildcard $(CLIENT_SRC_DIR)/*.cpp) \
    $(filter-out $(EXAMPLE_SRC_DIR)/main.cpp, $(wildcard $(EXAMPLE_SRC_DIR)/*.cpp))
LOCAL_C_INCLUDES := $(CLIENT_SRC_DIR) $(EXAMPLE_SRC_DIR)
LOCAL_CPPFLAGS := -std=c++17 -fexceptions -frtti -Wall
LOCAL_LDLIBS := -llog
LOCAL_LDFLAGS += -Wl,-z,max-page-size=16384
include $(BUILD_SHARED_LIBRARY)
