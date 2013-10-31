/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <mtdutils.h>
#include <mounts.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <make_ext4fs.h>
#include <ext4_utils.h>

#include "roots.h"
#include "libvolumeutils_ui.h"

int num_volumes = 0;
Volume* device_volumes = NULL;

extern struct selabel_handle *sehandle;

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef BLKDISCARD
#define BLKDISCARD _IO(0x12,119)
#endif

static int parse_options(char* options, Volume* volume) {
    char* option;
    if (!options || !volume) {
        LOGE("bad args to parse_options\n");
        return -1;
    }
    while ((option = strtok(options, ","))) {
        options = NULL;

        if (strncmp(option, "length=", 7) == 0) {
            volume->length = strtoll(option+7, NULL, 10);
        } else {
            LOGE("bad option \"%s\"\n", option);
            return -1;
        }
    }
    return 0;
}

void load_volume_table() {
    int alloc = 2;
    Volume* tmp_volumes = NULL;

    device_volumes = (Volume*)malloc(alloc * sizeof(Volume));

    if (device_volumes == NULL) {
        LOGE("failed to allocate device volumes (%s)\n", strerror(errno));
        return;
    }

    // Insert an entry for /tmp, which is the ramdisk and is always mounted.
    device_volumes[0].mount_point = "/tmp";
    device_volumes[0].fs_type = "ramdisk";
    device_volumes[0].device = NULL;
    device_volumes[0].device2 = NULL;
    device_volumes[0].length = 0;
    num_volumes = 1;

    FILE* fstab = fopen("/etc/recovery.fstab", "r");
    if (fstab == NULL) {
        LOGE("failed to open /etc/recovery.fstab (%s)\n", strerror(errno));
        return;
    }

    char buffer[1024];
    int i;
    while (fgets(buffer, sizeof(buffer)-1, fstab)) {
        for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
        if (buffer[i] == '\0' || buffer[i] == '#') continue;

        char* original = strdup(buffer);
        char* device = strtok(buffer+i, " \t\n");


        char* mount_point = strtok(NULL, " \t\n");
        char* fs_type = strtok(NULL, " \t\n");
        // lines may optionally have a second device, to use if
        // mounting the first one fails.
        char* options = NULL;
        strtok(NULL, " \t\n");
        char* device2 = strtok(NULL, " \t\n");
        if (device2) {
            if (device2[0] == '/') {
                options = strtok(NULL, " \t\n");
            } else {
                options = device2;
                device2 = NULL;
            }
        }

        if (mount_point && fs_type && device) {
            while (num_volumes >= alloc) {
                alloc *= 2;
                tmp_volumes = (Volume*)realloc(device_volumes, alloc*sizeof(Volume));

                if (tmp_volumes == NULL) {
                    LOGE("failed to reallocate device volumes (%s)\n", strerror(errno));
                    free(device_volumes);
                    device_volumes = NULL;
                    fclose(fstab);
                    return;
                }

                device_volumes = tmp_volumes;
            }
            device_volumes[num_volumes].mount_point = strdup(mount_point);
            device_volumes[num_volumes].fs_type = strdup(fs_type);
            device_volumes[num_volumes].device = strdup(device);
            device_volumes[num_volumes].device2 =
                    device2 ? strdup(device2) : NULL;

            device_volumes[num_volumes].length = 0;
            if (parse_options(options, device_volumes + num_volumes) != 0) {
                LOGE("skipping malformed recovery.fstab line: %s\n", original);
            } else {
                ++num_volumes;
            }
        } else {
            LOGE("skipping malformed recovery.fstab line: %s\n", original);
        }
        free(original);
    }

    fclose(fstab);

    LOGI("recovery filesystem table\n");
    LOGI("=========================\n");
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = &device_volumes[i];
        LOGI("  %d %s %s %s %s %lld\n", i, v->mount_point, v->fs_type,
               v->device, v->device2, v->length);
    }
    LOGI("\n");
}

Volume* volume_for_path(const char* path) {
    int i;
    if (!path) {
        LOGE("bad args to volume_for_path\n");
        return NULL;
    }
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = device_volumes+i;
        int len = strlen(v->mount_point);
        if (strncmp(path, v->mount_point, len) == 0 &&
                (path[len] == '\0' || path[len] == '/')) {
            return v;
        }
    }
    return NULL;
}

int ensure_path_mounted(const char* path) {
    Volume* v;

    if (!path) {
        LOGE("bad args to ensure_path_mounted\n");
        return -1;
    }

     v = volume_for_path(path);
     if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 0;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 0;
    }

    mkdir("/mnt", 0755);  // in case it doesn't already exist
    mkdir(v->mount_point, 0755);  // in case it doesn't already exist

    if (strcmp(v->fs_type, "yaffs2") == 0) {
        // mount an MTD partition as a YAFFS2 filesystem.
        mtd_scan_partitions();
        const MtdPartition* partition;
        partition = mtd_find_partition_by_name(v->device);
        if (partition == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->device, v->mount_point);
            return -1;
        }
        return mtd_mount_partition(partition, v->mount_point, v->fs_type, 0);
    } else if (strcmp(v->fs_type, "ext4") == 0 ||
               strcmp(v->fs_type, "vfat") == 0) {
        result = mount(v->device, v->mount_point, v->fs_type,
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
        if (result == 0) return 0;

        if (v->device2) {
            LOGW("failed to mount %s (%s); trying %s\n",
                 v->device, strerror(errno), v->device2);
            result = mount(v->device2, v->mount_point, v->fs_type,
                           MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
            if (result == 0) return 0;
        }

        LOGE("failed to mount %s (%s)\n", v->mount_point, strerror(errno));
        return -1;
    }

    LOGE("unknown fs_type \"%s\" for %s\n", v->fs_type, v->mount_point);
    return -1;
}

int ensure_path_unmounted(const char* path) {
    Volume* v;

    if (!path) {
        LOGE("bad args to ensure_path_unmounted\n");
        return -1;
    }

    v = volume_for_path(path);
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted; you can't unmount it.
        return -1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        // volume is already unmounted
        return 0;
    }

    return unmount_mounted_volume(mv);
}

int wipe_volume(const char *volume, int mode) {
    unsigned long long range[2];
    int result = 0;
    Volume* v;
    int discard_zeroes_data = 0;

    if (!volume) {
        LOGE("bad args to wipe_volume\n");
        return -1;
    }

    v = volume_for_path(volume);
    if (v == NULL) {
        LOGE("unknown volume \"%s\"\n", volume);
        return -1;
    }

    int fd = open(v->device, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) {
        LOGE("open failed on %s\n", v->device);
        return -1;
    }

    /* Check if it is emmc or HDD (do nothing) */
    if (strstr(v->device, "mmcblk") == NULL) {
        /* HDD disk */
        close(fd);
        return 0;
    }

    range[0] = 0;
    range[1] = get_file_size(fd);

    result = ioctl(fd, mode, &range);
    if (result < 0) {
        LOGE("BLK(%d)DISCARD failed on %s (size: %lld)\n", mode, v->device, range[1]);
        result = -1;
    }

    close(fd);

    return result;
}

int format_volume(const char* volume, void *sehandle) {
    Volume* v;

    if (!volume) {
        LOGE("bad args to format_volume\n");
        return -1;
    }

    v = volume_for_path(volume);
    if (v == NULL) {
        LOGE("unknown volume \"%s\"\n", volume);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", volume);
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
        LOGE("can't give path \"%s\" to format_volume\n", volume);
        return -1;
    }

    if (ensure_path_unmounted(volume) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(v->fs_type, "yaffs2") == 0 || strcmp(v->fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(v->device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", v->device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", v->device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", v->device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n", v->device);
            return -1;
        }
        return 0;
    }

    if (wipe_volume(volume, BLKDISCARD) < 0)
        return -1;

    if (strcmp(v->fs_type, "emmc") == 0) {
        return 0;
    }


    if (strcmp(v->fs_type, "ext4") == 0) {
        int result = make_ext4fs(v->device, v->length, volume, sehandle, 0);
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", v->device);
            return -1;
        }
        return 0;
    }

    LOGE("format_volume: fs_type \"%s\" unsupported\n", v->fs_type);
    return -1;
}
