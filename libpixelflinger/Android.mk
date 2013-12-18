LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

#
# C/C++ and ARMv5 objects
#

include $(CLEAR_VARS)
PIXELFLINGER_SRC_FILES:= \
    codeflinger/CodeCache.cpp \
    codeflinger/tinyutils/SharedBuffer.cpp \
    codeflinger/tinyutils/VectorImpl.cpp \
    format.cpp \
    clear.cpp \
    raster.cpp \
    buffer.cpp \
    scanline.cpp \
    fixed.cpp \
    picker.cpp \
    pixelflinger.cpp \
    trap.cpp

ifeq ($(TARGET_ARCH),arm)
PIXELFLINGER_SRC_FILES += codeflinger/arm/ARMAssemblerInterface.cpp \
    codeflinger/arm/ARMAssemblerProxy.cpp \
    codeflinger/arm/ARMAssembler.cpp \
    codeflinger/arm/GGLAssembler.cpp \
    codeflinger/arm/load_store.cpp \
    codeflinger/arm/blending.cpp \
    codeflinger/arm/texturing.cpp \
    codeflinger/arm/disassem.c
endif

ifeq ($(TARGET_ARCH),x86)

PIXELFLINGER_SRC_FILES +=  \
    codeflinger/x86/X86Assembler.cpp \
    codeflinger/x86/GGLX86Assembler.cpp \
    codeflinger/x86/load_store.cpp \
    codeflinger/x86/blending.cpp \
    codeflinger/x86/texturing.cpp

LOCAL_C_INCLUDES := \
    $(TOP)/vendor/intel/hardware/apache-harmony

LOCAL_STATIC_LIBRARIES += libenc
endif

ifeq ($(TARGET_ARCH),arm)
ifeq ($(TARGET_ARCH_VERSION),armv7-a)
PIXELFLINGER_SRC_FILES += col32cb16blend_neon.S
PIXELFLINGER_SRC_FILES += col32cb16blend.S
else
PIXELFLINGER_SRC_FILES += t32cb16blend.S
PIXELFLINGER_SRC_FILES += col32cb16blend.S
endif
endif

ifeq ($(TARGET_ARCH),arm)
# special optimization flags for pixelflinger
PIXELFLINGER_CFLAGS += -fstrict-aliasing -fomit-frame-pointer
endif

ifeq ($(TARGET_ARCH),mips)
PIXELFLINGER_SRC_FILES += codeflinger/MIPSAssembler.cpp
PIXELFLINGER_SRC_FILES += codeflinger/mips_disassem.c
PIXELFLINGER_SRC_FILES += arch-mips/t32cb16blend.S
PIXELFLINGER_CFLAGS += -fstrict-aliasing -fomit-frame-pointer
endif

LOCAL_SHARED_LIBRARIES := libcutils liblog

#
# Shared library
#

LOCAL_MODULE:= libpixelflinger
LOCAL_SRC_FILES := $(PIXELFLINGER_SRC_FILES)
LOCAL_CFLAGS := $(PIXELFLINGER_CFLAGS)

ifneq ($(BUILD_TINY_ANDROID),true)
# Really this should go away entirely or at least not depend on
# libhardware, but this at least gets us built.
LOCAL_SHARED_LIBRARIES += libhardware_legacy
LOCAL_CFLAGS += -DWITH_LIB_HARDWARE
endif
include $(BUILD_SHARED_LIBRARY)

#
# Static library version
#

include $(CLEAR_VARS)
LOCAL_MODULE:= libpixelflinger_static
LOCAL_SRC_FILES := $(PIXELFLINGER_SRC_FILES)
LOCAL_CFLAGS := $(PIXELFLINGER_CFLAGS) 
ifeq ($(TARGET_ARCH),x86)
LOCAL_C_INCLUDES := \
    $(TOP)/vendor/intel/hardware/apache-harmony
LOCAL_STATIC_LIBRARIES += libenc
endif
include $(BUILD_STATIC_LIBRARY)


include $(call all-makefiles-under,$(LOCAL_PATH))
