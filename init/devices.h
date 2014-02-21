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

#ifndef _INIT_DEVICES_H
#define _INIT_DEVICES_H

#include <sys/stat.h>

#define MAX_DEV_PATH 512

extern void handle_device_fd();
extern void handle_modalias_triggers(const char* modalias);
extern void device_init(void);
extern int module_probe(const char *alias);
extern int add_dev_perms(const char *name, const char *attr,
                         mode_t perm, unsigned int uid,
                         unsigned int gid, unsigned short wildcard);
extern int add_usb_device_class_matching(
                         const char *devclass,
                         mode_t perm, unsigned int uid,
                         unsigned int gid, const char* options);

extern int add_mod_args(int nargs, const char *mod_name, char *args[]);
extern int add_inet_args(char *net_link, char *if_name, char *target_name);
extern int add_dev_args(unsigned int vid, unsigned int pid, char *dev_name, char *target_name);

int get_device_fd();
void coldboot(const char *path);
#endif	/* _INIT_DEVICES_H */
