/*
 * Copyright 2011, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <sys/reboot.h>
#include <dirent.h>

static struct signal_set signal_array[] = {
    {SIGUSR2, ANDROID_RB_POWEROFF, ""},
    {SIGTERM, ANDROID_RB_RESTART, ""},
    {SIGHUP, ANDROID_RB_RESTART2, "android"},
    {SIGINT, ANDROID_RB_RESTART2, "recovery"},
    {SIGQUIT, ANDROID_RB_RESTART2, "bootloader"},
    {SIGTSTP, ANDROID_RB_RESTART2, "fastboot"},
};

/* Check to see if /proc/mounts contains any writeable filesystems
 * backed by a block device.
 * Return true if none found, else return false.
 */
static int remount_ro_done(void)
{
    FILE *f;
    char mount_dev[256];
    char mount_dir[256];
    char mount_type[256];
    char mount_opts[256];
    int mount_freq;
    int mount_passno;
    int match;
    int found_rw_fs = 0;

    f = fopen("/proc/mounts", "r");
    if (! f) {
        /* If we can't read /proc/mounts, just give up */
        return 1;
    }

    do {
        match = fscanf(f, "%255s %255s %255s %255s %d %d\n",
                       mount_dev, mount_dir, mount_type,
                       mount_opts, &mount_freq, &mount_passno);
        mount_dev[255] = 0;
        mount_dir[255] = 0;
        mount_type[255] = 0;
        mount_opts[255] = 0;
        if ((match == 6) && !strncmp(mount_dev, "/dev/block", 10) && strstr(mount_opts, "rw")) {
            found_rw_fs = 1;
            break;
        }
    } while (match != EOF);

    fclose(f);

    return !found_rw_fs;
}

/* Remounting filesystems read-only is difficult when there are files
 * opened for writing or pending deletes on the filesystem.  There is
 * no way to force the remount with the mount(2) syscall.  The magic sysrq
 * 'u' command does an emergency remount read-only on all writable filesystems
 * that have a block device (i.e. not tmpfs filesystems) by calling
 * emergency_remount(), which knows how to force the remount to read-only.
 * Unfortunately, that is asynchronous, and just schedules the work and
 * returns.  The best way to determine if it is done is to read /proc/mounts
 * repeatedly until there are no more writable filesystems mounted on
 * block devices.
 */
static void remount_ro(void)
{
    int fd, cnt = 0;

    /* Trigger the remount of the filesystems as read-only,
     * which also marks them clean.
     */
    fd = open("/proc/sysrq-trigger", O_WRONLY);
    if (fd < 0) {
        return;
    }
    write(fd, "u", 1);
    close(fd);


    /* Now poll /proc/mounts till it's done */
    while (!remount_ro_done() && (cnt < 50)) {
        usleep(100000);
        cnt++;
    }

    return;
}

static int check_user_task(int pid)
{
    int fd,r;
    char cmdline[1024];

    // Ignore /init
    if(pid == 1)
        return 0;

    snprintf(cmdline, sizeof(cmdline), "/proc/%d/cmdline", pid);
    fd = open(cmdline, O_RDONLY);
    if(fd == 0) {
        r = 0;
    } else {
        r = read(fd, cmdline, 1023);
        close(fd);
        if(r < 0) r = 0;
    }
    cmdline[r] = 0;

    // Exclude kernel thread, ia_watchdogd
    if(r == 0 || strstr(cmdline, "ia_watchdogd"))
        return 0;
    return 1;
}

static int check_process_running(pid_t *pid_array, int pid_count,
                                 pid_t *pid_running)
{
    int i, running = 0;
    char name[1024];
    DIR *d;

    for(i = 0;i < pid_count; i++) {
        snprintf(name, sizeof(name), "/proc/%d/", pid_array[i]);
        d = opendir(name);
        if(d != 0) {
            pid_running[running] = pid_array[i];
            running++;
            closedir(d);
        }
    }
    return running;
}

/* kill all user spaces processes
 * This is a workground because that "ia_watchdogd" is killed may
 * cause immediate system reboot, so exclude it.
 */
void kill_user_space_tasks(void)
{
    DIR *d;
    struct dirent *de;
    pid_t pids[2048];
    pid_t pidrunning[2048];
    int pidcount = 0, i;
    int running, sleepcount = 0;

    /*kill(-1, SIGTERM);
    sync();
    sleep(1);
    kill(-1, SIGKILL);
    sync();
    sleep(1);*/

    d = opendir("/proc/");
    if(d == 0) return;

    while((de = readdir(d)) != 0){
        if(isdigit(de->d_name[0])) {
            int pid = atoi(de->d_name);
            if(check_user_task(pid)) {
	       pids[pidcount++]=pid;
               pidcount %= 2048;
            }
       }
    }
    closedir(d);

    for(i=0;i<pidcount;i++) {
        kill(pids[i], SIGUSR1);
        kill(pids[i], SIGUSR2);
        kill(pids[i], SIGTERM);
        kill(pids[i], SIGKILL);
    }
    KLOG_ERROR("init", "Sent SIGTERM & SIGKILL to all processes!");

    running = pidcount;
    while(running > 0 && sleepcount < 5) {
        running = check_process_running(pids, pidcount, pidrunning);
        sleepcount++;
        sleep(1);
    }
    for(i = 0;i < running; i++) {
         KLOG_ERROR("init", "pid: %d is still alive\n", pidrunning[i]);
    }
    KLOG_ERROR("init", "%d/%d processes are killed (%d seconds)", pidcount - running, pidcount, sleepcount);

    return;
}

static int write_sig(int cmd, char *arg)
{
    unsigned int i;
    for (i = 0; i < sizeof(signal_array)/sizeof(signal_array[0]); i++) {
        struct signal_set *ss = &signal_array[i];
        if(cmd == ss->cmd) {
            if(arg == NULL)
                return ss->sig;
            if(!strcmp(arg, ss->arg))
                return ss->sig;
        }
    }
    if (cmd == ANDROID_RB_RESTART)
        return SIGTERM;
    if (cmd == ANDROID_RB_POWEROFF)
        return SIGUSR2;
    if (cmd == ANDROID_RB_RESTART2)
        return SIGHUP;
    return SIGUSR2;
}

void read_sig(int sig, int *cmd, char *arg)
{
    unsigned int i;
    for (i = 0; i < sizeof(signal_array)/sizeof(signal_array[0]); i++) {
        struct signal_set *ss = &signal_array[i];
        if(sig == ss->sig) {
            *cmd = ss->cmd;
            strlcpy(arg, ss->arg, 64);
            return;
        }
    }
    *cmd = ANDROID_RB_POWEROFF;
    strlcpy(arg, "", 64);
}

void install_signal_handler(void(*f)(int))
{
    unsigned int i;
    for (i = 0; i < sizeof(signal_array)/sizeof(signal_array[0]); i++) {
        struct signal_set *ss = &signal_array[i];
        signal(ss->sig, f);
    }
}

/*reset signal handlers and unblock signals*/
void reset_signal_handler(void)
{
    unsigned int i;
    sigset_t set;
    for (i = 0; i < sizeof(signal_array)/sizeof(signal_array[0]); i++) {
        struct signal_set *ss = &signal_array[i];
        signal(ss->sig, SIG_DFL);
    }
    signal(SIGCHLD, SIG_IGN);
    sigfillset(&set);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

int really_reboot(int cmd, char *arg)
{
    int ret;

    switch (cmd) {
        case ANDROID_RB_RESTART:
            ret = reboot(RB_AUTOBOOT);
            break;

        case ANDROID_RB_POWEROFF:
            ret = reboot(RB_POWER_OFF);
            break;

        case ANDROID_RB_RESTART2:
            ret = __reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                           LINUX_REBOOT_CMD_RESTART2, arg);
            break;

        default:
            ret = -1;
    }
    return ret;
}

int android_reboot(int cmd, int flags, char *arg)
{
    int sig = write_sig(cmd, arg);

    /* Send SIGUSR1 to init set reboot cmd and arg */
    kill(1, SIGUSR1);

    if (!(flags & ANDROID_RB_FLAG_NO_SYNC))
        sync();
    if (!(flags & ANDROID_RB_FLAG_NO_REMOUNT_RO))
        remount_ro();

    /* Send sig to init to reboot/shutdown system */
    kill(1, sig);
    return 0;
}

