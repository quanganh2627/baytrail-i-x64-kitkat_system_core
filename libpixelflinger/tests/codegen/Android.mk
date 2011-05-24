LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	codegen.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libpixelflinger

LOCAL_C_INCLUDES := \
	system/core/libpixelflinger

ifeq ($(TARGET_ARCH),x86)
LOCAL_C_INCLUDES += \
	$(TOP)/vendor/intel/hardware/apache-harmony
endif

LOCAL_MODULE:= test-opengl-codegen

LOCAL_MODULE_TAGS := tests

include $(BUILD_EXECUTABLE)
