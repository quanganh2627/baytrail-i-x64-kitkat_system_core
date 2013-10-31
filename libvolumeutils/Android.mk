#
# Copyright (C) 2010 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(my-dir)

# Shared library for target
# ========================================================

include $(CLEAR_VARS)

RECOVERY_FSTAB_VERSION := 2
LOCAL_MODULE := libvolumeutils
LOCAL_SRC_FILES := \
    roots.c \
    ufdisk.c

LOCAL_C_INCLUDES += system/core/mtdutils \
        system/extras/ext4_utils \
        bionic/libc/private

LOCAL_SHARED_LIBRARIES := liblog libext4_utils
LOCAL_STATIC_LIBRARIES := libmtdutils libc libcutils liblogwrap

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

RECOVERY_FSTAB_VERSION := 2
LOCAL_MODULE := libvolumeutils_static
LOCAL_SRC_FILES := \
    roots.c \
    ufdisk.c

LOCAL_C_INCLUDES += system/core/mtdutils \
        system/extras/ext4_utils \
        bionic/libc/private

LOCAL_STATIC_LIBRARIES := libmtdutils libc libcutils liblog libext4_utils_static libz liblogwrap

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

RECOVERY_FSTAB_VERSION := 2
LOCAL_MODULE := libvolumeutils_ui_static
LOCAL_SRC_FILES := \
    roots.c \
    ufdisk.c

LOCAL_CFLAGS += -DUSE_GUI

LOCAL_C_INCLUDES += system/core/mtdutils \
        system/extras/ext4_utils \
        bionic/libc/private

LOCAL_STATIC_LIBRARIES := libmtdutils libc libcutils liblog libext4_utils_static libz liblogwrap

include $(BUILD_STATIC_LIBRARY)
