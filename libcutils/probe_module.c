/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <cutils/misc.h>
#include <cutils/list.h>
#include <cutils/module_parsers.h>
#include <cutils/probe_module.h>

#define LOG_TAG "ProbeModule"
#include <cutils/log.h>


#define LDM_DEFAULT_DEP_FILE "/lib/modules/modules.dep"
#define LDM_DEFAULT_MOD_PATH "/lib/modules/"
#define LDM_INIT_DEP_NUM 10
#define LDM_DEFAULT_LINE_SZ 1024

extern int init_module(void *, unsigned long, const char *);
extern int delete_module(const char *, unsigned int);

static void dump_dep(char **dep)
{
    int d;

    for (d = 0; dep[d]; d++)
        ALOGD("DUMP DEP: %s\n", dep[d]);
}

static char * strip_path(const char * const str)
{
    char *ptr;
    int i;

    /* initialize pos to terminator */
    for (i = strlen(str); i > 0; i--)
        if (str[i - 1] == '/')
            break;

    return (char *)&str[i];
}

static void hyphen_to_underscore(char *str)
{
    while (str && *str != '\0') {
        if (*str == '-')
            *str = '_';
        str++;
    }
}

/* Compare module names, but don't differentiate '_' and '-'.
 * return: 0 when s1 is matched to s2 or size is zero.
 *         non-zero in any other cases.
 */
static int match_name(const char *s1, const char *s2, const size_t size)
{
    size_t i;

    if (!size)
        return 0;

    for (i = 0; i < size; i++, s1++, s2++) {

        if ((*s1 == '_' || *s1 == '-') && (*s2 == '_' || *s2 == '-'))
            continue;

        if (*s1 != *s2)
            return -1;

        if (*s1 == '\0')
            return 0;
    }

    return 0;
}

/* check if a line in dep file is target module's dependency.
 * return 1 when it is, otherwise 0 in any other cases.
 */
#define SUFFIX_SIZE 3
static int is_target_module(char *line, const char *target, size_t line_size)
{
    char *token;
    size_t name_len;
    int ret = 0;
    char *s;

    /* search : */
    token = strchr(line, ':');

    if (!token || token > line + line_size) {
        ALOGE("invalid line: no token\n");
        return 0;
    }
    /* go backward to first / */
    for (s = token; *s != '/' && s > line ; s--);

    if (*s == '/')
        s++;

    name_len = strlen(target);

    /* check length */
    if (s + name_len + SUFFIX_SIZE != token)
        return 0;

    /* check basename */
    if (match_name(s, target, name_len))
        return 0;

    /* check suffix */
    if (strncmp(".ko", &s[name_len], SUFFIX_SIZE))
        return 0;

    return 1;
}

/* turn a single string into an array of dependency.
 *
 * return: an array of pointer to each dependency,
 * followed by the dependency strings. so free(dep) is
 * enough to free everything.
 */
static char** setup_dep(char *line, size_t line_size)
{
    char *start;
    char *end;
    int dep_num = 0;
    int i;
    char **dep = NULL;
    char *deplist;
    size_t len;

    /* Count the dependency (by counting space)
     * to allocate the right size for the array
     */
    start = line;
    end = strchr(start, ' ');
    for (dep_num = 1; end != NULL && end < line + line_size; end = strchr(start, ' ')) {
        dep_num++;
        start = end + 1;
    }

    /* allocate the dep pointer table and the strings at once */
    dep = malloc(sizeof(*dep) * (dep_num + 1) + line_size + 1);
    if (!dep) {
        ALOGE("cannot alloc dep array\n");
        return dep;
    }

    /* put the deplist after the pointer table */
    deplist = (char *) &dep[dep_num + 1];

    /* Now copy the line from modules.dep to a list of strings :
     * main_mod.ko: dep1.ko dep2.ko
     * into :
     * main_mod.ko\0\0dep1.ko\0dep2.ko\0
     * and update pointer array to the beginning of each string.
     */
    dep[0] = deplist;
    memcpy(deplist, line, line_size);
    deplist[line_size] = '\0';

    start = deplist;
    for (i = 1 ; i < dep_num ; i++) {
        end = strchr(start, ' ');
        if (!end)
            goto err;
        *end = '\0';
        start = end + 1;
        dep[i] = start;
    }
    /* remove ':' for main module */
    end = strchr(deplist, ':');
    if (end)
        *end = '\0';

    /* terminate array with a null pointer */
    dep[dep_num] = NULL;
    return dep;

    err:
    ALOGE("%s Error when parsing modules.dep\n", __FUNCTION__);
    free(dep);
    return NULL;
}

static int insmod(const char *path_name, const char *args)
{
    void *data;
    unsigned int len;
    int ret;

    data = load_file(path_name, &len);

    if (!data) {
        ALOGE("%s: Failed to load module file [%s]\n", __FUNCTION__, path_name);
        return -1;
    }

    ret = init_module(data, len, args);

    if (ret != 0 && errno != EEXIST) {
        ALOGE("%s: Failed to insmod [%s] with args [%s] error: %s ret: %d\n",
                __FUNCTION__, path_name, args, strerror(errno), ret);
        ret = -1;
    }
    else
        ret = 0;    /* if module is already in kernel, return success. */

    free(data);

    return ret;
}

/* install all modules in the dependency chain
 * deps    : A array of module file names, must be terminated by a NULL pointer
 * args    : The module parameters for target module.
 * strip   : Non-zero to strip out path info in the file name;
 *           0 to keep path info when loading modules.
 * base    : a prefix to module path, it will NOT be affected by strip flag.
 * return  : 0 for success or nothing to do; non-zero when any error occurs.
 */
int insmod_s(char *dep[], const char *args, int strip, const char *base)
{
    char *name;
    char *path_name;
    int cnt;
    size_t len;
    int ret = 0;
    const char * base_dir = LDM_DEFAULT_MOD_PATH;

    if (base && strlen(base))
        base_dir = base;

    /* load modules in reversed order */
    for (cnt = 0; dep[cnt]; cnt++)
        ;

    while (cnt--) {

        name = strip ? strip_path(dep[cnt]) : dep[cnt];

        len = strlen(base_dir) + strlen(name) + 1;

        path_name = malloc(sizeof(char) * len);

        if (!path_name) {
            ALOGE("alloc module [%s] path failed\n", path_name);
            return -1;
        }

        snprintf(path_name, len, "%s%s", base_dir, name);

        if (cnt)
            ret = insmod(path_name, "");
        else
            ret = insmod(path_name, args);

        free(path_name);

        if (ret)
            break;
    }

    return ret;
}

static int rmmod(const char *mod_name, unsigned int flags)
{
    return delete_module(mod_name, flags);
}

/* remove all modules in a dependency chain
 * NOTE: We assume module name in kernel is same as the file name without .ko
 */
static int rmmod_s(char *dep[], unsigned int flags)
{
    int i;
    int ret = 0;
    char * mod_name;

    for (i = 0; dep[i]; i++) {
        size_t len;
        mod_name = strip_path(dep[i]);
        len = strlen(mod_name);

        if (len > strlen(".ko")
                && mod_name[len - 1] == 'o'
                        && mod_name[len - 2] == 'k'
                                && mod_name[len - 3] == '.') {

            mod_name[len - 3] = '\0';

            hyphen_to_underscore(mod_name);

            ret = rmmod(mod_name, flags);

            if (ret) {
                ALOGE("%s: Failed to remove module [%s] error (%s)\n",
                        __FUNCTION__, mod_name, strerror(errno));
                break;

            }
        }
    }

    return ret;
}

/* look_up_dep() find and setup target module's dependency in modules.dep
 *
 * dep_file:    a pointer to module's dep file loaded in memory, its content
 *              won't be changed, so it can be reused after parsing.
 *
 * return:      a pointer to an array which holds the dependency strings and
 *              terminated by a NULL pointer. Caller is responsible to free the
 *              array's memory and also the array items' memory.
 *
 *              non-zero in any other cases. Content of dep array is invalid.
 */
static char ** look_up_dep(const char *module_name, void *dep_file)
{
    unsigned int line_size;
    char *start;
    char *end;
    char **dep;

    if (!dep_file || !module_name || *module_name == '\0')
        return NULL;

    start = (char *)dep_file;

    /* We expect modules.dep file has a new line char before EOF. */
    while ((end = strchr(start, '\n')) != NULL) {
        line_size = (end - start);
        if (is_target_module(start, module_name, line_size)) {
            dep = setup_dep(start, line_size);
            return dep;
        }
        start = end + 1;
    }
    return NULL;
}

/* load_dep_file() load a dep file (usually it is modules.dep)
 * into memory. Caller is responsible to free the memory.
 *
 * file_name:   dep file's name, if it is NULL or an empty string,
 *              This function will try to load a dep file in the
 *              default path defined in LDM_DEFAULT_DEP_FILE
 *
 * return:      a pointer to the allocated mem which holds all
 *              content of the depfile. a zero pointer will be
 *              returned for any errors.
 * */
static void *load_dep_file(const char *file_name)
{
    const char *dep_file_name = LDM_DEFAULT_DEP_FILE;
    unsigned int len;

    if (file_name && *file_name != '\0')
        dep_file_name = file_name;

    return load_file(dep_file_name, &len);
}

/* is_dep_in_blacklist() checks if any module in dependency
 * is in a blacklilst
 * dep:         dependency array
 * blacklist :  head of a black list.
 * return:      1 if any module in dep is in black list.
 *              -1 when any error happens
 *              0 none of modules in dep is in black list.
 * */
static int is_dep_in_blacklist(char *dep[], struct listnode *blacklist)
{
    int i;
    char *tmp;
    int ret = 0;
    size_t len;

    for (i = 0; dep[i]; i++) {
        tmp = dep[i];
        len = strlen(tmp);

        if (!(len > strlen(".ko")
                && tmp[len - 1] == 'o'
                        && tmp[len - 2] == 'k'
                                && tmp[len - 3] == '.')) {
            ret = -1;

            break;
        }

        if (asprintf(&tmp, "%s", dep[i]) <= 0) {
            ret = -1;

            break;
        }

        tmp[len - 3] = '\0';
        if (is_module_blacklisted(strip_path(tmp), blacklist)) {
            ALOGE("found module [%s] is in black list\n", tmp);
            free(tmp);
            ret = 1;

            break;
        }
        free(tmp);
    }

    return ret;
}
static void dump_black_list(struct listnode *black_list_head)
{
    struct listnode *blklst_node;
    struct module_blacklist_node *blacklist;

    list_for_each(blklst_node, black_list_head) {
        blacklist = node_to_item(blklst_node,
                                 struct module_blacklist_node,
                                 list);

            ALOGE("DUMP BLACK: [%s]\n", blacklist->name);
    }
}

static int validate_module(const char *module_name, char *dep_file, struct listnode *extra_blacklist, char ***dep)
{

    *dep = look_up_dep(module_name, dep_file);

    if (!(*dep)) {
        ALOGE("%s: cannot find module's dependency info: [%s]\n", __FUNCTION__, module_name);
        return MOD_DEP_NOT_FOUND;
    }

    if (is_dep_in_blacklist(*dep, extra_blacklist)) {
        ALOGE("%s: a module is in caller's black list, stop further loading\n", __FUNCTION__);
        free(*dep);
        return MOD_IN_CALLER_BLACK;
    }
    return 0;
}

int get_module_dep(const char *module_name,
        const char *dep_name,
        int cached,
        const char *blacklist,
        char ***dep)
{
    static void *dep_file = NULL;

    int i = 0;
    struct listnode *alias_node;
    struct module_alias_node *alias;
    static list_declare(extra_blacklist);
    static list_declare(alias_list);
    list_declare(module_aliases);
    int ret;

    ret = MOD_UNKNOWN;

    if (!module_name || *module_name == '\0') {
        ALOGE("need valid module name\n");
        return MOD_INVALID_NAME;
    }

    if (!cached || list_empty(&alias_list)) {
        ret = parse_alias_to_list("/lib/modules/modules.alias", &alias_list);

        if (ret) {
            ALOGE("%s: parse alias error %d\n", __FUNCTION__, ret);
            free_alias_list(&alias_list);
            return MOD_BAD_ALIAS;
        }
    }

    if (blacklist && *blacklist != '\0') {
        if (!cached || list_empty(&extra_blacklist)) {
            ret = parse_blacklist_to_list(blacklist, &extra_blacklist);
            if (ret) {
                ALOGI("%s: parse extra black list error %d\n", __FUNCTION__, ret);

                /* A black list from caller is optional, but when caller does
                 * give us a file and something's wrong with it, we will stop going further*/
                ret = MOD_INVALID_CALLER_BLACK;
                free_black_list(&extra_blacklist);
                goto free_file;
            }
        }
    }

    if (!cached || dep_name || !dep_file)
        dep_file = load_dep_file(dep_name);

    if (!dep_file) {
        ALOGE("cannot load dep file\n");
        ret = MOD_BAD_DEP;

        goto free_file;
    }

    /* check if module name is an alias. */
    if (get_module_name_from_alias(module_name, &module_aliases, &alias_list) <= 0) {
        ret = validate_module(module_name, dep_file, &extra_blacklist, dep);
    } else {
        list_for_each(alias_node, &module_aliases) {
            alias = node_to_item(alias_node, struct module_alias_node, list);

            ret = validate_module(alias->name, dep_file, &extra_blacklist, dep);

            if (ret == 0) {
                goto free_file;
            }
        }
    }

free_file:
    if (!cached) {
        free(dep_file);
        dep_file = NULL;
        free_alias_list(&alias_list);
        free_black_list(&extra_blacklist);
    }
    free_alias_list(&module_aliases);
    return ret;
}

/* insmod_by_dep() interface to outside,
 * refer to its description in probe_module.h
 */
int insmod_by_dep(const char *module_name,
        const char *args,
        const char *dep_name,
        int strip,
        const char *base,
        const char *blacklist)
{
    char **dep;
    int ret;

    ret = get_module_dep(module_name, dep_name, 0, blacklist, &dep);

    if (ret)
        return ret;

    ret = insmod_s(dep, args, strip, base);

    free(dep);
    return ret;
}

/* rmmod_by_dep() interface to outside,
 * refer to its description in probe_module.h
 */
int rmmod_by_dep(const char *module_name,
        const char *dep_name)
{
    void *dep_file;
    char **dep = NULL;
    int ret = MOD_UNKNOWN;

    if (!module_name || *module_name == '\0') {
        ALOGE("need valid module name\n");
        ret = MOD_INVALID_NAME;

        return ret;
    }

    dep_file = load_dep_file(dep_name);

    if (!dep_file) {
        ALOGE("cannot load dep file : %s\n", dep_name);
        ret = MOD_BAD_DEP;

        return ret;
    }

    dep = look_up_dep(module_name, dep_file);

    if (!dep) {
        ALOGE("%s: cannot remove module: [%s]\n", __FUNCTION__, module_name);
        ret = MOD_DEP_NOT_FOUND;

        goto free_file;
    }

    ret = rmmod_s(dep, O_NONBLOCK);

    free(dep);

free_file:
    free(dep_file);

    return ret;
}

/* end of file */
