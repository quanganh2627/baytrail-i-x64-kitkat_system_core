# Copyright 2006 The Android Open Source Project

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= logcat.cpp event.logtags

LOCAL_SHARED_LIBRARIES := liblog

LOCAL_MODULE:= logcat

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE:= logcat_static

LOCAL_FORCE_STATIC_EXECUTABLE := true

# Set path to avoid logcat_static installed under system directory
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/sbin

LOCAL_SRC_FILES:= logcat.cpp event.logtags

LOCAL_STATIC_LIBRARIES := liblog libc libstdc++

include $(BUILD_EXECUTABLE)
