LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE:= test-opengl-codegen
LOCAL_MODULE_TAGS := tests

LOCAL_SRC_FILES := \
    codegen.cpp

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libpixelflinger

LOCAL_C_INCLUDES := \
    system/core/libpixelflinger

ifeq ($(TARGET_ARCH),x86)
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/libenc
endif

include $(BUILD_EXECUTABLE)
