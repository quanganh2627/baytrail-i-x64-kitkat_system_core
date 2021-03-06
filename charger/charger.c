/*
 * Copyright (C) 2011 The Android Open Source Project
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

//#define DEBUG_UEVENTS
#define CHARGER_KLOG_LEVEL 6

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/rtc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/socket.h>
#include <linux/netlink.h>

#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <cutils/list.h>
#include <cutils/misc.h>
#include <cutils/uevent.h>
#include <cutils/properties.h>

#ifdef CHARGER_ENABLE_SUSPEND
#include <suspend/autosuspend.h>
#endif

#include "minui/minui.h"

#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

#define ARRAY_SIZE(x)           (sizeof(x)/sizeof(x[0]))

#define MSEC_PER_SEC            (1000LL)
#define NSEC_PER_MSEC           (1000000LL)

#define BATTERY_UNKNOWN_TIME    (2 * MSEC_PER_SEC)
#define POWER_ON_KEY_TIME       (2 * MSEC_PER_SEC)
#define UNPLUGGED_SHUTDOWN_TIME (10 * MSEC_PER_SEC)
#define THERMAL_POLL_TIME       (5 * MSEC_PER_SEC)

#define BATTERY_FULL_THRESH     95
#define BOOT_BATT_MIN_CAP_THRS  3

#define LAST_KMSG_PATH          "/proc/last_kmsg"
#define LAST_KMSG_MAX_SZ        (32 * 1024)

#define LOGE(x...) do { KLOG_ERROR("charger", x); } while (0)
#define LOGI(x...) do { KLOG_INFO("charger", x); } while (0)
#define LOGV(x...) do { KLOG_DEBUG("charger", x); } while (0)

#define TEMP_BASE_PATH               "/sys/class/thermal/thermal_zone"
#define TEMP_SENS_TYPE               "/type"
#define TEMP_SENS_VAL                "/temp"
#define TEMP_MON_TYPE_FRONT_SKIN     "skin0"
#define TEMP_MON_TYPE_BACK_SKIN      "skin1"
#define TEMP_MON_TYPE_BATTERY        "battery"

/* temperature is in mC */
#define CRIT_TEMP_THRESH_FRONT_SKIN  64000
#define CRIT_TEMP_THRESH_BACK_SKIN   74000
#define CRIT_TEMP_THRESH_BATTERY     60000

#define RTC_FILE                 "/dev/rtc0"
#define IPC_DEVICE_NAME          "/dev/mid_ipc"
#define IPC_WRITE_ALARM_TO_OSNIB 0xC5
#define ALARM_SET                1
#define ALARM_CLEAR              0

#define INVALID_BATT_MODEL "UNKNOWN"
#define STATUS_CHARGING "Charging"
#define STATUS_FULL "Full"

struct key_state {
    bool pending;
    bool down;
    int64_t timestamp;
};

struct power_supply {
    struct listnode list;
    char name[256];
    char type[32];
    bool online;
    bool valid;
    char cap_path[PATH_MAX];
    char model_path[PATH_MAX];
    char charge_status_path[PATH_MAX];
};

struct frame {
    const char *name;
    int disp_time;
    int min_capacity;
    bool level_only;

    gr_surface surface;
};

struct animation {
    bool run;

    struct frame *frames;
    int cur_frame;
    int num_frames;

    int cur_cycle;
    int num_cycles;

    int anim_thresh;

    /* current capacity being animated */
    int capacity;
};

struct charger {
    int64_t next_screen_transition;
    int64_t next_key_check;
    int64_t next_pwr_check;

    struct key_state keys[KEY_MAX + 1];
    int uevent_fd;

    struct listnode supplies;
    int num_supplies;
    int num_supplies_online;

    struct animation *batt_anim;
    gr_surface surf_unknown;

    struct power_supply *battery;

    int boot_min_cap;
};

struct uevent {
    const char *action;
    const char *path;
    const char *subsystem;
    const char *ps_name;
    const char *ps_type;
    const char *ps_online;
};

static struct frame batt_anim_frames[] = {
    {
        .name = "charger/battery_crit",
        .disp_time = 750,
        .min_capacity = 0,
    },
    {
        .name = "charger/battery_0",
        .disp_time = 750,
        .min_capacity = 0,
    },
    {
        .name = "charger/battery_0a",
        .disp_time = 750,
        .min_capacity = 20,
    },
    {
        .name = "charger/battery_1",
        .disp_time = 750,
        .min_capacity = 20,
    },
    {
        .name = "charger/battery_1a",
        .disp_time = 750,
        .min_capacity = 40,
    },
    {
        .name = "charger/battery_2",
        .disp_time = 750,
        .min_capacity = 40,
    },
    {
        .name = "charger/battery_3",
        .disp_time = 750,
        .min_capacity = 60,
    },
    {
        .name = "charger/battery_4",
        .disp_time = 750,
        .min_capacity = 80,
    },
    {
        .name = "charger/battery_5",
        .disp_time = 750,
        .min_capacity = BATTERY_FULL_THRESH,
    },
};

static struct animation battery_animation = {
    .frames = batt_anim_frames,
    .num_frames = ARRAY_SIZE(batt_anim_frames),
    .num_cycles = 3,
};

static struct charger charger_state = {
    .batt_anim = &battery_animation,
};

static int char_width;
static int char_height;

/* current time in milliseconds */
static int64_t curr_time_ms(void)
{
    struct timespec tm;
    clock_gettime(CLOCK_MONOTONIC, &tm);
    return tm.tv_sec * MSEC_PER_SEC + (tm.tv_nsec / NSEC_PER_MSEC);
}

static void clear_screen(void)
{
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());
};

#define MAX_KLOG_WRITE_BUF_SZ 256

static void dump_last_kmsg(void)
{
    char *buf;
    char *ptr;
    unsigned sz = 0;
    int len;

    LOGI("\n");
    LOGI("*************** LAST KMSG ***************\n");
    LOGI("\n");
    buf = load_file(LAST_KMSG_PATH, &sz);
    if (!buf || !sz) {
        LOGI("last_kmsg not found. Cold reset?\n");
        goto out;
    }

    len = min(sz, LAST_KMSG_MAX_SZ);
    ptr = buf + (sz - len);

    while (len > 0) {
        int cnt = min(len, MAX_KLOG_WRITE_BUF_SZ);
        char yoink;
        char *nl;

        nl = memrchr(ptr, '\n', cnt - 1);
        if (nl)
            cnt = nl - ptr + 1;

        yoink = ptr[cnt];
        ptr[cnt] = '\0';
        klog_write(6, "<6>%s", ptr);
        ptr[cnt] = yoink;

        len -= cnt;
        ptr += cnt;
    }

    free(buf);

out:
    LOGI("\n");
    LOGI("************* END LAST KMSG *************\n");
    LOGI("\n");
}

static int read_file(const char *path, char *buf, size_t sz)
{
    int fd;
    size_t cnt;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        goto err;

    cnt = read(fd, buf, sz - 1);
    if (cnt <= 0)
        goto err;
    buf[cnt] = '\0';
    if (buf[cnt - 1] == '\n') {
        cnt--;
        buf[cnt] = '\0';
    }

    close(fd);
    return cnt;

err:
    if (fd >= 0)
        close(fd);
    return -1;
}

static int read_file_int(const char *path, int *val)
{
    char buf[32];
    int ret;
    int tmp;
    char *end;

    ret = read_file(path, buf, sizeof(buf));
    if (ret < 0)
        return -1;

    tmp = strtol(buf, &end, 0);
    if (end == buf ||
        ((end < buf+sizeof(buf)) && (*end != '\n' && *end != '\0')))
        goto err;

    *val = tmp;
    return 0;

err:
    return -1;
}

static int get_battery_capacity(struct charger *charger)
{
    int ret;
    int batt_cap = -1;

    if (!charger->battery)
        return -1;

    ret = read_file_int(charger->battery->cap_path, &batt_cap);
    if (ret < 0 || batt_cap > 100) {
        batt_cap = -1;
    }

    return batt_cap;
}

static int is_battery_valid(struct charger *charger)
{
    int ret;
    char model_name[32];

    if (!charger->battery)
        return -1;

    ret = read_file(charger->battery->model_path, model_name, sizeof(model_name));
    if (ret < 0)
        return -1;

    if (!strncmp(model_name, INVALID_BATT_MODEL, strlen(INVALID_BATT_MODEL)))
        return 0;
    else
        return 1;
}

static int is_status_charging(struct charger *charger)
{
    int ret;
    char charge_status[32];

    if (!charger->battery)
        return 0;

    ret = read_file(charger->battery->charge_status_path, charge_status, sizeof(charge_status));
    if (ret < 0)
        return 0;

    if (!strncmp(charge_status, STATUS_CHARGING, strlen(STATUS_CHARGING))
            || !strncmp(charge_status, STATUS_FULL, strlen(STATUS_FULL)))
        return 1;
    else
        return 0;
}

static struct power_supply *find_supply(struct charger *charger,
                                        const char *name)
{
    struct listnode *node;
    struct power_supply *supply;

    list_for_each(node, &charger->supplies) {
        supply = node_to_item(node, struct power_supply, list);
        if (!strncmp(name, supply->name, sizeof(supply->name)))
            return supply;
    }
    return NULL;
}

static struct power_supply *add_supply(struct charger *charger,
                                       const char *name, const char *type,
                                       const char *path, bool online)
{
    struct power_supply *supply;

    supply = calloc(1, sizeof(struct power_supply));
    if (!supply)
        return NULL;

    strlcpy(supply->name, name, sizeof(supply->name));
    strlcpy(supply->type, type, sizeof(supply->type));
    snprintf(supply->cap_path, sizeof(supply->cap_path),
             "/sys/%s/capacity", path);
    snprintf(supply->model_path, sizeof(supply->model_path),
             "/sys/%s/model_name", path);
    snprintf(supply->charge_status_path, sizeof(supply->charge_status_path),
             "/sys/%s/status", path);
    supply->online = online;
    list_add_tail(&charger->supplies, &supply->list);
    charger->num_supplies++;
    LOGI("... added %s %s %d\n", supply->name, supply->type, online);
    return supply;
}

static void remove_supply(struct charger *charger, struct power_supply *supply)
{
    if (!supply)
        return;
    list_remove(&supply->list);
    charger->num_supplies--;
    free(supply);
}

#ifdef CHARGER_ENABLE_SUSPEND
static int request_suspend(bool enable)
{
    if (enable)
        return autosuspend_enable();
    else
        return autosuspend_disable();
}
#else
static int request_suspend(bool enable)
{
    return 0;
}
#endif

static void parse_uevent(const char *msg, struct uevent *uevent)
{
    uevent->action = "";
    uevent->path = "";
    uevent->subsystem = "";
    uevent->ps_name = "";
    uevent->ps_online = "";
    uevent->ps_type = "";

    /* currently ignoring SEQNUM */
    while (*msg) {
#ifdef DEBUG_UEVENTS
        LOGV("uevent str: %s\n", msg);
#endif
        if (!strncmp(msg, "ACTION=", 7)) {
            msg += 7;
            uevent->action = msg;
        } else if (!strncmp(msg, "DEVPATH=", 8)) {
            msg += 8;
            uevent->path = msg;
        } else if (!strncmp(msg, "SUBSYSTEM=", 10)) {
            msg += 10;
            uevent->subsystem = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_NAME=", 18)) {
            msg += 18;
            uevent->ps_name = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_ONLINE=", 20)) {
            msg += 20;
            uevent->ps_online = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_TYPE=", 18)) {
            msg += 18;
            uevent->ps_type = msg;
        }

        /* advance to after the next \0 */
        while (*msg++)
            ;
    }

    LOGV("event { '%s', '%s', '%s', '%s', '%s', '%s' }\n",
         uevent->action, uevent->path, uevent->subsystem,
         uevent->ps_name, uevent->ps_type, uevent->ps_online);
}

static void process_ps_uevent(struct charger *charger, struct uevent *uevent)
{
    int online;
    char ps_type[32], str_online[3];
    struct power_supply *supply = NULL;
    int i;
    bool was_online = false;
    bool battery = false;
    struct listnode *node;
    char path_online[PATH_MAX];

    if (uevent->ps_type[0] == '\0') {
        char *path;
        int ret;

        if (uevent->path[0] == '\0')
            return;
        ret = asprintf(&path, "/sys/%s/type", uevent->path);
        if (ret <= 0)
            return;
        ret = read_file(path, ps_type, sizeof(ps_type));
        free(path);
        if (ret < 0)
            return;
    } else {
        strlcpy(ps_type, uevent->ps_type, sizeof(ps_type));
    }

    if (!strncmp(ps_type, "Battery", 7))
        battery = true;

    online = atoi(uevent->ps_online);
    supply = find_supply(charger, uevent->ps_name);

    if (!strcmp(uevent->action, "add")) {
        if (!supply) {
            supply = add_supply(charger, uevent->ps_name, ps_type, uevent->path,
                                online);
            if (!supply) {
                LOGE("cannot add supply '%s' (%s %d)\n", uevent->ps_name,
                    uevent->ps_type, online);
                return;
            }

            /* only pick up the first battery for now */
            if (battery && !charger->battery)
                charger->battery = supply;

            /* update with online charge sources as we are adding it to the supply list */
            if (!battery && online)
                charger->num_supplies_online++;

        } else {
            LOGE("supply '%s' already exists..\n", uevent->ps_name);
        }
    } else if (!strcmp(uevent->action, "remove")) {
        if (supply) {
            if (charger->battery == supply)
                charger->battery = NULL;
            remove_supply(charger, supply);
            supply = NULL;
        }
    } else if (!strcmp(uevent->action, "change")) {
        if (!supply) {
            LOGE("power supply '%s' not found ('%s' %d)\n",
                 uevent->ps_name, ps_type, online);
            return;
        }
    } else {
        return;
    }

    /* allow battery to be managed in the supply list but make it not
     * contribute to online power supplies. */
    /* For every PSY change loop through all avalilable charge sources */
    list_for_each(node, &charger->supplies) {
        supply = node_to_item(node, struct power_supply, list);
        if (strncmp("Battery", supply->type, sizeof(supply->type))) {
            sprintf(path_online,"/sys/class/power_supply/%s/online", supply->name);
            if (read_file(path_online, str_online, sizeof(str_online))<0) {
                LOGI("online attribute is NULL for %s\n",supply->name);
                continue;
            }

            online = atoi(str_online);
            was_online = supply->online;
            supply->online = online;
            if (was_online && !online) {
                charger->num_supplies_online--;
            } else if (!was_online && online) {
                charger->num_supplies_online++;
            }
        }
    }

    LOGI("power supply %s (%s) %s (action=%s num_online=%d num_supplies=%d)\n",
         uevent->ps_name, ps_type, battery ? "" : online ? "online" : "offline",
         uevent->action, charger->num_supplies_online, charger->num_supplies);
}

static void process_uevent(struct charger *charger, struct uevent *uevent)
{
    if (!strcmp(uevent->subsystem, "power_supply"))
        process_ps_uevent(charger, uevent);
}

#define UEVENT_MSG_LEN  1024
static int handle_uevent_fd(struct charger *charger, int fd)
{
    char msg[UEVENT_MSG_LEN+2];
    int n;

    if (fd < 0)
        return -1;

    while (true) {
        struct uevent uevent;

        n = uevent_kernel_multicast_recv(fd, msg, UEVENT_MSG_LEN);
        if (n <= 0)
            break;
        if (n >= UEVENT_MSG_LEN)   /* overflow -- discard */
            continue;

        msg[n] = '\0';
        msg[n+1] = '\0';

        parse_uevent(msg, &uevent);
        process_uevent(charger, &uevent);
    }

    return 0;
}

static int uevent_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;

    if (!(revents & POLLIN))
        return -1;
    return handle_uevent_fd(charger, fd);
}

/* force the kernel to regenerate the change events for the existing
 * devices, if valid */
static void do_coldboot(struct charger *charger, DIR *d, const char *event,
                        bool follow_links, int max_depth)
{
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY);
    if (fd >= 0) {
        write(fd, event, strlen(event));
        close(fd);
        handle_uevent_fd(charger, charger->uevent_fd);
    }

    while ((de = readdir(d)) && max_depth > 0) {
        DIR *d2;

        LOGV("looking at '%s'\n", de->d_name);

        if ((de->d_type != DT_DIR && !(de->d_type == DT_LNK && follow_links)) ||
           de->d_name[0] == '.') {
            LOGV("skipping '%s' type %d (depth=%d follow=%d)\n",
                 de->d_name, de->d_type, max_depth, follow_links);
            continue;
        }
        LOGV("can descend into '%s'\n", de->d_name);

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            LOGE("cannot openat %d '%s' (%d: %s)\n", dfd, de->d_name,
                 errno, strerror(errno));
            continue;
        }

        d2 = fdopendir(fd);
        if (d2 == 0)
            close(fd);
        else {
            LOGV("opened '%s'\n", de->d_name);
            do_coldboot(charger, d2, event, follow_links, max_depth - 1);
            closedir(d2);
        }
    }
}

static void coldboot(struct charger *charger, const char *path,
                     const char *event)
{
    char str[256];

    LOGV("doing coldboot '%s' in '%s'\n", event, path);
    DIR *d = opendir(path);
    if (d) {
        snprintf(str, sizeof(str), "%s\n", event);
        do_coldboot(charger, d, str, true, 1);
        closedir(d);
    }
}

static int draw_text(const char *str, int x, int y)
{
    int str_len_px = gr_measure(str);

    if (x < 0)
        x = (gr_fb_width() - str_len_px) / 2;
    if (y < 0)
        y = (gr_fb_height() - char_height) / 2;
    gr_text(x, y, str, 0);

    return y + char_height;
}

static void android_green(void)
{
    gr_color(0xa4, 0xc6, 0x39, 255);
}

/* returns the last y-offset of where the surface ends */
static int draw_surface_centered(struct charger *charger, gr_surface surface)
{
    int w;
    int h;
    int x;
    int y;

    w = gr_get_width(surface);
    h = gr_get_height(surface);
    x = (gr_fb_width() - w) / 2 ;
    y = (gr_fb_height() - h) / 2 ;

    LOGV("drawing surface %dx%d+%d+%d\n", w, h, x, y);
    gr_blit(surface, 0, 0, w, h, x, y);
    return y + h;
}

static void draw_unknown(struct charger *charger)
{
    int y;
    if (charger->surf_unknown) {
        draw_surface_centered(charger, charger->surf_unknown);
    } else {
        android_green();
        y = draw_text("Charging!", -1, -1);
        draw_text("?\?/100", -1, y + 25);
    }
}

static void draw_battery(struct charger *charger)
{
    struct animation *batt_anim = charger->batt_anim;
    struct frame *frame = &batt_anim->frames[batt_anim->cur_frame];

    if (batt_anim->num_frames != 0) {
        draw_surface_centered(charger, frame->surface);
        LOGV("drawing frame #%d name=%s min_cap=%d time=%d\n",
             batt_anim->cur_frame, frame->name, frame->min_capacity,
             frame->disp_time);

        if (get_battery_capacity(charger) < charger->boot_min_cap) {
            struct frame *crit_frame = &batt_anim->frames[0];
            draw_surface_centered(charger, crit_frame->surface);
            LOGV("drawing battery_crit frame\n");
        }
    }
}

static void redraw_screen(struct charger *charger)
{
    struct animation *batt_anim = charger->batt_anim;

    clear_screen();

    /* try to display *something* */
    if (batt_anim->capacity < 0 || batt_anim->num_frames == 0
        || !is_battery_valid(charger))
        draw_unknown(charger);
    else
        draw_battery(charger);
    gr_flip();
}

static void kick_animation(struct animation *anim)
{
    anim->run = true;
}

static void reset_animation(struct animation *anim)
{
    anim->cur_cycle = 0;
    anim->cur_frame = 0;
    anim->run = false;
}

static void update_screen_state(struct charger *charger, int64_t now)
{
    struct animation *batt_anim = charger->batt_anim;
    int cur_frame;
    int disp_time;
    int batt_cap;

    if (!batt_anim->run || now < charger->next_screen_transition)
        return;

    /* animation is over, blank screen and leave */
    if (batt_anim->cur_cycle == batt_anim->num_cycles) {
        reset_animation(batt_anim);
        charger->next_screen_transition = -1;
        gr_fb_blank(true);
        LOGV("[%lld] animation done\n", now);

        /* Stop at the correct-level, as animation could have
           ended at the next level */
        batt_cap = get_battery_capacity(charger);
        if (batt_cap < batt_anim->frames[batt_anim->anim_thresh].min_capacity)
            batt_anim->cur_frame = batt_anim->anim_thresh - 1;
        else
            batt_anim->cur_frame = batt_anim->anim_thresh;
        redraw_screen(charger);
        reset_animation(batt_anim);

        if (charger->num_supplies_online > 0 && is_status_charging(charger)) {
            request_suspend(true);
            clear_screen();
            gr_flip();
        }
        return;
    }

    disp_time = batt_anim->frames[batt_anim->cur_frame].disp_time;

    /* animation starting, set up the animation */
    if (batt_anim->cur_frame == 0) {
        int ret;

        LOGV("[%lld] animation starting\n", now);
        batt_cap = get_battery_capacity(charger);
        if (batt_cap >= 0 && batt_anim->num_frames != 0) {
            int i;

            /* find first frame given current capacity */
            for (i = 1; i < batt_anim->num_frames; i++) {
                if (batt_cap < batt_anim->frames[i].min_capacity)
                    break;
            }
            batt_anim->cur_frame = i - 1;
            /* Run animation only till the next segment */
            if (i == batt_anim->num_frames)
                batt_anim->anim_thresh = batt_anim->cur_frame;
            else
                batt_anim->anim_thresh = batt_anim->cur_frame + 1;

            /* show the first frame for twice as long */
            disp_time = batt_anim->frames[batt_anim->cur_frame].disp_time * 2;
        }

        batt_anim->capacity = batt_cap;
    }

    /* unblank the screen  on first cycle */
    if (batt_anim->cur_cycle == 0)
        gr_fb_blank(false);

    /* draw the new frame (@ cur_frame) */
    redraw_screen(charger);

    /* if we don't have anim frames, we only have one image, so just bump
     * the cycle counter and exit
     */
    if (batt_anim->num_frames == 0 || batt_anim->capacity < 0) {
        LOGV("[%lld] animation missing or unknown battery status\n", now);
        charger->next_screen_transition = now + BATTERY_UNKNOWN_TIME;
        batt_anim->cur_cycle++;
        return;
    }

    /* schedule next screen transition */
    charger->next_screen_transition = now + disp_time;

    /* advance frame cntr to the next valid frame only if we are charging
     * if necessary, advance cycle cntr, and reset frame cntr
     */
    if (charger->num_supplies_online != 0 && is_status_charging(charger)) {
        batt_anim->cur_frame++;

        /* if the frame is used for level-only, that is only show it when it's
         * the current level, skip it during the animation.
         */
        while (batt_anim->cur_frame < batt_anim->num_frames &&
               batt_anim->frames[batt_anim->cur_frame].level_only)
            batt_anim->cur_frame++;
        if (batt_anim->cur_frame > batt_anim->anim_thresh) {
            batt_anim->cur_cycle++;
            batt_anim->cur_frame = 0;

        /* don't reset the cycle counter, since we use that as a signal
         * in a test above to check if animation is over
         */
        }
    } else {
        /* Stop animating if we're not charging.
         * If we stop it immediately instead of going through this loop, then
         * the animation would stop somewhere in the middle.
         */
        batt_anim->cur_frame = 0;
        batt_anim->cur_cycle++;
    }
}

static int set_key_callback(int code, int value, void *data)
{
    struct charger *charger = data;
    int64_t now = curr_time_ms();
    int down = !!value;

    if (code > KEY_MAX)
        return -1;

    /* ignore events that don't modify our state */
    if (charger->keys[code].down == down)
        return 0;

    /* only record the down even timestamp, as the amount
     * of time the key spent not being pressed is not useful */
    if (down)
        charger->keys[code].timestamp = now;
    charger->keys[code].down = down;
    charger->keys[code].pending = true;
    if (down) {
        LOGV("[%lld] key[%d] down\n", now, code);
    } else {
        int64_t duration = now - charger->keys[code].timestamp;
        int64_t secs = duration / 1000;
        int64_t msecs = duration - secs * 1000;
        LOGV("[%lld] key[%d] up (was down for %lld.%lldsec)\n", now,
            code, secs, msecs);
    }

    return 0;
}

static void update_input_state(struct charger *charger,
                               struct input_event *ev)
{
    if (ev->type != EV_KEY)
        return;
    set_key_callback(ev->code, ev->value, charger);
}

static void set_next_key_check(struct charger *charger,
                               struct key_state *key,
                               int64_t timeout)
{
    int64_t then = key->timestamp + timeout;

    if (charger->next_key_check == -1 || then < charger->next_key_check)
        charger->next_key_check = then;
}

static void process_key(struct charger *charger, int code, int64_t now)
{
    struct key_state *key = &charger->keys[code];
    int64_t next_key_check;

    if (code == KEY_POWER) {
        if (key->down) {
            int64_t reboot_timeout = key->timestamp + POWER_ON_KEY_TIME;
            if (now >= reboot_timeout) {
                if (get_battery_capacity(charger) >= charger->boot_min_cap) {
                    LOGI("[%lld] rebooting\n", now);
                    android_reboot(ANDROID_RB_RESTART, 0, 0);
                } else {
                    LOGI("[%lld] ignore power-button press, battery level "
                            "less than minimum\n", now);
                }
            } else {
                /* if the key is pressed but timeout hasn't expired,
                 * make sure we wake up at the right-ish time to check
                 */
                set_next_key_check(charger, key, POWER_ON_KEY_TIME);
            }
            kick_animation(charger->batt_anim);
            request_suspend(false);
        } else {
            /* if the power key got released, force screen state cycle */
            if (key->pending)
                kick_animation(charger->batt_anim);
        }
    }

    key->pending = false;
}

static void handle_input_state(struct charger *charger, int64_t now)
{
    process_key(charger, KEY_POWER, now);

    if (charger->next_key_check != -1 && now > charger->next_key_check)
        charger->next_key_check = -1;
}

static void handle_power_supply_state(struct charger *charger, int64_t now)
{
    if (charger->num_supplies_online == 0 || !is_status_charging(charger)
            || !is_battery_valid(charger)) {
        kick_animation(charger->batt_anim);
        request_suspend(false);
        if (charger->next_pwr_check == -1) {
            charger->next_pwr_check = now + UNPLUGGED_SHUTDOWN_TIME;
            LOGI("[%lld] device unplugged or invalid battery: shutting down in %lld (@ %lld)\n",
                 now, UNPLUGGED_SHUTDOWN_TIME, charger->next_pwr_check);
        } else if (now >= charger->next_pwr_check) {
            LOGI("[%lld] shutting down\n", now);
            if (!is_battery_valid(charger))
                system("echo 1 > /sys/module/intel_mid_osip/parameters/force_shutdown_occured");
            android_reboot(ANDROID_RB_POWEROFF, 0, 0);
        } else {
            /* otherwise we already have a shutdown timer scheduled */
        }
    } else {
        /* online supply present, reset shutdown timer if set */
        if (charger->next_pwr_check != -1) {
            LOGI("[%lld] device plugged in: shutdown cancelled\n", now);
            kick_animation(charger->batt_anim);
        }
        charger->next_pwr_check = -1;
    }
}

static int get_temp_interface(char *sensor_name)
{
    int sensor_count = 0;
    char path[PATH_MAX], buf[PATH_MAX];
    int ret = -1;
    static int index_skin0 = -1;
    static int index_skin1 = -1;
    static int index_battery = -1;

    if (!sensor_name)
        return ret;

    /* if the sysfs path is found already, just return with value */
    if ((!strcmp(sensor_name, "skin0") || !strcmp(sensor_name, "SYSTHERM0")) && index_skin0 != -1)
        return index_skin0;
    else if ((!strcmp(sensor_name, "skin1") || !strcmp(sensor_name, "SYSTHERM1")) && index_skin1 != -1)
        return index_skin1;
    else if (strstr(sensor_name, "battery") && index_battery != -1)
        return index_battery;

    snprintf(path, sizeof(path), "%s%d%s", TEMP_BASE_PATH, sensor_count, TEMP_SENS_TYPE);
    memset(buf, 0, sizeof(buf));
    /* loop through all the sysfs files. Exit when file doesnt exist.
     * Assumption is if file doesnt exist for a given sensor_count,
     * no file exist for a higher sensor_count */
    while (read_file(path, buf, sizeof(buf)) >= 0) {
         if (strstr(buf, sensor_name)){
             ret = sensor_count;
             break;
         }

         sensor_count++;
         snprintf(path, sizeof(path), "%s%d%s", TEMP_BASE_PATH, sensor_count, TEMP_SENS_TYPE);
         memset(buf, 0, sizeof(buf));
    }

    if (ret == -1)
        return ret;

    if (!strcmp(sensor_name, "skin0") || !strcmp(sensor_name, "SYSTHERM0"))
        index_skin0 = ret;
    else if (!strcmp(sensor_name, "skin1") || !strcmp(sensor_name, "SYSTHERM1"))
        index_skin1 = ret;
    else if (strstr(sensor_name, "battery"))
        index_battery = ret;

    return ret;
}

static void handle_temperature_state(struct charger *charger)
{
    int temp_front, temp_back, temp_battery;
    int sensor_type_front, sensor_type_back, sensor_type_battery;
    int ret;
    char path_front[PATH_MAX], path_back[PATH_MAX], path_battery[PATH_MAX];

    sensor_type_front = get_temp_interface(TEMP_MON_TYPE_FRONT_SKIN);
    if (sensor_type_front < 0) {
        sensor_type_front = get_temp_interface("SYSTHERM1");
        if (sensor_type_front < 0)
           return;
    }

    sensor_type_back = get_temp_interface(TEMP_MON_TYPE_BACK_SKIN);
    if (sensor_type_back < 0) {
        sensor_type_back = get_temp_interface("SYSTHERM0");
        if (sensor_type_back < 0)
           return;
    }

    sensor_type_battery = get_temp_interface(TEMP_MON_TYPE_BATTERY);
    if (sensor_type_battery < 0)
        return;

    snprintf(path_front, sizeof(path_front), "%s%d%s", TEMP_BASE_PATH,
             sensor_type_front, TEMP_SENS_VAL);


    ret = read_file_int(path_front, &temp_front);
    if (ret < 0) {
        LOGE("Unable to open/read file %s\n", path_front);
        return;
    }

    snprintf(path_back, sizeof(path_back), "%s%d%s", TEMP_BASE_PATH,
             sensor_type_back, TEMP_SENS_VAL);

    ret = read_file_int(path_back, &temp_back);
    if (ret < 0) {
        LOGE("Unable to open/read file %s\n", path_back);
        return;
    }

    snprintf(path_battery, sizeof(path_battery), "%s%d%s", TEMP_BASE_PATH,
             sensor_type_battery, TEMP_SENS_VAL);

    ret = read_file_int(path_battery, &temp_battery);
    if (ret < 0) {
        LOGE("Unable to open/read file %s\n", path_battery);
        return;
    }

    if (temp_front >= CRIT_TEMP_THRESH_FRONT_SKIN ||
        temp_back >= CRIT_TEMP_THRESH_BACK_SKIN ||
        temp_battery >= CRIT_TEMP_THRESH_BATTERY) {
        kick_animation(charger->batt_anim);
        request_suspend(false);

        LOGI("Temperature threshold breached: Front_skin_temp:%d, Back_skin_temp:%d, Battery_temp:%d\n"
             "Thresholds: Front:%d, Back:%d, Battery:%d\nShutting down system\n", temp_front, temp_back,
             temp_battery, CRIT_TEMP_THRESH_FRONT_SKIN, CRIT_TEMP_THRESH_BACK_SKIN, CRIT_TEMP_THRESH_BATTERY);

        system("echo 1 > /sys/module/intel_mid_osip/parameters/force_shutdown_occured");
        android_reboot(ANDROID_RB_POWEROFF, 0, 0);
    }
}

int write_alarm_to_osnib(int mode)
{
    int devfd, errNo, ret;

    devfd = open(IPC_DEVICE_NAME, O_RDWR);
    if (devfd < 0) {
        LOGE("unable to open the DEVICE %s\n", IPC_DEVICE_NAME);
        ret = -1;
        goto err1;
    }

    errNo = ioctl(devfd, IPC_WRITE_ALARM_TO_OSNIB, &mode);
    if (errNo < 0) {
        LOGE("ioctl for DEVICE %s, returns error-%d\n",
                        IPC_DEVICE_NAME, errNo);
        ret = -1;
        goto err2;
    }
    ret = 0;

err2:
    close(devfd);
err1:
    return ret;
}

void *handle_rtc_alarm_event(void *arg)
{
    struct charger *charger = (struct charger *) arg;
    unsigned long data;
    int rtc_fd, ret;
    int batt_cap;
    struct rtc_wkalrm alarm;

    write_alarm_to_osnib(ALARM_CLEAR);

    rtc_fd = open(RTC_FILE, O_RDONLY, 0);
    if (rtc_fd < 0) {
        LOGE("Unable to open the DEVICE %s\n", RTC_FILE);
        goto err1;
    }

    /* RTC alarm set ? */
    ret = ioctl(rtc_fd, RTC_WKALM_RD, &alarm);
    if (ret == -1) {
        LOGE("ioctl(RTC_WKALM_RD) failed\n");
        goto err2;
    }

    if (!alarm.enabled)
        LOGI("No RTC wake-alarm set\n");
    else {
        LOGI("RTC wake-alarm set: %04d-%02d-%02d %02d:%02d:%02d\n",
                alarm.time.tm_year+1900,
                alarm.time.tm_mon+1,
                alarm.time.tm_mday,
                alarm.time.tm_hour,
                alarm.time.tm_min,
                alarm.time.tm_sec);

        /* Enable alarm interrupts */
        ret = ioctl(rtc_fd, RTC_AIE_ON, 0);
        if (ret == -1) {
            LOGE("rtc ioctl RTC_AIE_ON error\n");
            goto err2;
        }
    }

    /* This blocks until the alarm ring causes an interrupt */
    ret = read(rtc_fd, &data, sizeof(unsigned long));
    if (ret < 0) {
        LOGE("rtc read error\n");
        goto err2;
    }

    batt_cap = get_battery_capacity(charger);
    if (batt_cap >= charger->boot_min_cap) {
        LOGI("RTC alarm rang, Rebooting to MOS");

        if (write_alarm_to_osnib(ALARM_SET))
            LOGE("Error in setting alarm-flag to OSNIB");

        android_reboot(ANDROID_RB_RESTART, 0, 0);
    } else {
        LOGI("RTC alarm rang, capacity:%d less than minimum threshold:%d, "
             "cannot boot to MOS", batt_cap, charger->boot_min_cap);
    }

err2:
    close(rtc_fd);
err1:
    return NULL;
}

static void wait_next_event(struct charger *charger, int64_t now)
{
    int64_t next_event = INT64_MAX;
    int64_t timeout;
    struct input_event ev;
    int ret;

    LOGV("[%lld] next screen: %lld next key: %lld next pwr: %lld\n", now,
         charger->next_screen_transition, charger->next_key_check,
         charger->next_pwr_check);

    if (charger->next_screen_transition != -1)
        next_event = charger->next_screen_transition;
    if (charger->next_key_check != -1 && charger->next_key_check < next_event)
        next_event = charger->next_key_check;
    if (charger->next_pwr_check != -1 && charger->next_pwr_check < next_event)
        next_event = charger->next_pwr_check;

    if (next_event != -1 && next_event != INT64_MAX)
        timeout = max(0, next_event - now);
    else
        timeout = THERMAL_POLL_TIME;
    LOGV("[%lld] blocking (%lld)\n", now, timeout);
    ret = ev_wait((int)timeout);
    if (!ret)
        ev_dispatch();
}

static int input_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;
    struct input_event ev;
    int ret;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;
    update_input_state(charger, &ev);
    return 0;
}

static void event_loop(struct charger *charger)
{
    int ret;

    while (true) {
        int64_t now = curr_time_ms();

        LOGV("[%lld] event_loop()\n", now);
        handle_input_state(charger, now);
        handle_power_supply_state(charger, now);
        handle_temperature_state(charger);

        /* do screen update last in case any of the above want to start
         * screen transitions (animations, etc)
         */
        update_screen_state(charger, now);

        wait_next_event(charger, now);
    }
}

int main(int argc, char **argv)
{
    int ret;
    struct charger *charger = &charger_state;
    int64_t now = curr_time_ms() - 1;
    int fd;
    int i;
    char value[PROPERTY_VALUE_MAX], default_value[PROPERTY_VALUE_MAX];
    pthread_t t;

    list_init(&charger->supplies);

    klog_init();
    klog_set_level(CHARGER_KLOG_LEVEL);

    dump_last_kmsg();

    LOGI("--------------- STARTING CHARGER MODE ---------------\n");

    gr_init();
    gr_font_size(&char_width, &char_height);

    if (pthread_create(&t, NULL, handle_rtc_alarm_event, charger) != 0)
        LOGE("Error in creating rtc-alarm thread\n");

    ev_init(input_callback, charger);

    fd = uevent_open_socket(64*1024, true);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        ev_add_fd(fd, uevent_callback, charger);
    }
    charger->uevent_fd = fd;
    coldboot(charger, "/sys/class/power_supply", "add");

    ret = res_create_surface("charger/battery_fail", &charger->surf_unknown);
    if (ret < 0) {
        LOGE("Cannot load image\n");
        charger->surf_unknown = NULL;
    }

    for (i = 0; i < charger->batt_anim->num_frames; i++) {
        struct frame *frame = &charger->batt_anim->frames[i];

        ret = res_create_surface(frame->name, &frame->surface);
        if (ret < 0) {
            LOGE("Cannot load image %s\n", frame->name);
            /* TODO: free the already allocated surfaces... */
            charger->batt_anim->num_frames = 0;
            charger->batt_anim->num_cycles = 1;
            break;
        }
    }

    ev_sync_key_state(set_key_callback, charger);

    sprintf(default_value, "%d", BOOT_BATT_MIN_CAP_THRS);
    property_get("ro.boot.min.cap", value, default_value);
    sscanf(value, "%d", &charger->boot_min_cap);
    LOGI("Minimum capacity for MOS-boot:%d\n", charger->boot_min_cap);

#ifndef CHARGER_DISABLE_INIT_BLANK
    gr_fb_blank(true);
#endif

    charger->next_screen_transition = now - 1;
    charger->next_key_check = -1;
    charger->next_pwr_check = -1;
    reset_animation(charger->batt_anim);
    kick_animation(charger->batt_anim);

    event_loop(charger);

    return 0;
}
