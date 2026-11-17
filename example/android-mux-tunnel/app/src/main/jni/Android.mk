# Two shared libraries are built:
#   libhev-socks5-tunnel.so  tun2socks engine (git submodule third_party/hev-socks5-tunnel)
#   libmuxtunnel.so          netRectorClient + mux-tunnelClient core + JNI bridge
MY_PATH := $(call my-dir)
REPO_ROOT := $(abspath $(MY_PATH)/../../../../../..)
HEV_DIR := $(REPO_ROOT)/third_party/hev-socks5-tunnel

ifeq ($(wildcard $(HEV_DIR)/Android.mk),)
$(error hev-socks5-tunnel is missing: run "git submodule update --init --recursive")
endif

# hev-socks5-tunnel (also defines its own yaml, lwip and hev-task-system modules)
include $(HEV_DIR)/Android.mk

# BoringSSL for TLS (libssl.a / libcrypto.a per ABI), built by scripts/build-boringssl-android.sh into
# example/android-mux-tunnel/boringssl. Without it the app is built WITHOUT TLS ("Enable TLS" then reports an error).
# BORINGSSL_CUSTOM=1: the BoringSSL has SSL_set_record_protocol_version / TLS_CL_VERSION (a stock one has not).
BSSL_DIR := $(MY_PATH)/../../../../boringssl
ifneq ($(wildcard $(BSSL_DIR)/include/openssl/ssl.h),)
BSSL_FOUND := 1
include $(CLEAR_VARS)
LOCAL_MODULE := bssl_ssl
LOCAL_SRC_FILES := $(BSSL_DIR)/lib/$(TARGET_ARCH_ABI)/libssl.a
include $(PREBUILT_STATIC_LIBRARY)
include $(CLEAR_VARS)
LOCAL_MODULE := bssl_crypto
LOCAL_SRC_FILES := $(BSSL_DIR)/lib/$(TARGET_ARCH_ABI)/libcrypto.a
include $(PREBUILT_STATIC_LIBRARY)
else
$(warning BoringSSL not found in $(BSSL_DIR): building WITHOUT TLS (run scripts/build-boringssl-android.sh))
endif

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
ifdef BSSL_FOUND
LOCAL_C_INCLUDES += $(BSSL_DIR)/include
LOCAL_CPPFLAGS += -DNETRECTOR_HAS_TLS=1
ifeq ($(BORINGSSL_CUSTOM),1)
LOCAL_CPPFLAGS += -DNETRECTOR_BSSL_RECORD_VERSION=1
endif
LOCAL_STATIC_LIBRARIES += bssl_ssl bssl_crypto
endif
LOCAL_LDLIBS := -llog
LOCAL_LDFLAGS += -Wl,-z,max-page-size=16384
include $(BUILD_SHARED_LIBRARY)
