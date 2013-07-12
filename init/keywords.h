
#ifndef KEYWORD
int do_builtin_coldboot(int nargs, char **args);
int do_chroot(int nargs, char **args);
int do_chdir(int nargs, char **args);
int do_class_start(int nargs, char **args);
int do_class_stop(int nargs, char **args);
int do_class_reset(int nargs, char **args);
int do_domainname(int nargs, char **args);
int do_exec(int nargs, char **args);
int do_export(int nargs, char **args);
int do_hostname(int nargs, char **args);
int do_ifup(int nargs, char **args);
int do_insmod(int nargs, char **args);
int do_mkdir(int nargs, char **args);
int do_mount_all(int nargs, char **args);
int do_mount(int nargs, char **args);
int do_probemod(int nargs, char **args);
int do_restart(int nargs, char **args);
int do_restorecon(int nargs, char **args);
int do_rm(int nargs, char **args);
int do_rmdir(int nargs, char **args);
int do_setcon(int nargs, char **args);
int do_setenforce(int nargs, char **args);
int do_setkey(int nargs, char **args);
int do_setprop(int nargs, char **args);
int do_ext_setprop(int nargs, char **args);
int do_setrlimit(int nargs, char **args);
int do_setsebool(int nargs, char **args);
int do_start(int nargs, char **args);
int do_stop(int nargs, char **args);
int do_trigger(int nargs, char **args);
int do_symlink(int nargs, char **args);
int do_sysclktz(int nargs, char **args);
int do_write(int nargs, char **args);
int do_setprop_from_sysfs(int nargs, char **args);
int do_copy(int nargs, char **args);
int do_chown(int nargs, char **args);
int do_chmod(int nargs, char **args);
int do_loglevel(int nargs, char **args);
int do_load_persist_props(int nargs, char **args);
int do_wait(int nargs, char **args);
#define __MAKE_KEYWORD_ENUM__
#define KEYWORD(symbol, flags, nargs, func, uev_func) K_##symbol,
enum {
    K_UNKNOWN,
#endif
    KEYWORD(capability,  OPTION,  0, 0, 0)
    KEYWORD(chdir,       COMMAND, 1, do_chdir, 0)
    KEYWORD(chroot,      COMMAND, 1, do_chroot, 0)
    KEYWORD(class,       OPTION,  0, 0, 0)
    KEYWORD(class_start, COMMAND, 1, do_class_start, 0)
    KEYWORD(class_stop,  COMMAND, 1, do_class_stop, 0)
    KEYWORD(class_reset, COMMAND, 1, do_class_reset, 0)
    KEYWORD(console,     OPTION,  0, 0, 0)
    KEYWORD(critical,    OPTION,  0, 0, 0)
    KEYWORD(disabled,    OPTION,  0, 0, 0)
    KEYWORD(domainname,  COMMAND, 1, do_domainname, 0)
    KEYWORD(exec,        COMMAND, 1, do_exec, 0)
    KEYWORD(export,      COMMAND, 2, do_export, 0)
    KEYWORD(group,       OPTION,  0, 0, 0)
    KEYWORD(hostname,    COMMAND, 1, do_hostname, 0)
    KEYWORD(ifup,        COMMAND, 1, do_ifup, 0)
    KEYWORD(insmod,      COMMAND, 1, do_insmod, 0)
    KEYWORD(import,      SECTION, 1, 0, 0)
    KEYWORD(keycodes,    OPTION,  0, 0, 0)
    KEYWORD(mkdir,       COMMAND, 1, do_mkdir, 0)
    KEYWORD(mount_all,   COMMAND, 1, do_mount_all, 0)
    KEYWORD(mount,       COMMAND, 3, do_mount, 0)
    KEYWORD(on,          SECTION, 0, 0, 0)
    KEYWORD(oneshot,     OPTION,  0, 0, 0)
    KEYWORD(onrestart,   OPTION,  0, 0, 0)
    KEYWORD(probemod,    COMMAND, 1, do_probemod, 0)
    KEYWORD(restart,     COMMAND, 1, do_restart, 0)
    KEYWORD(restorecon,  COMMAND, 1, do_restorecon, 0)
    KEYWORD(rm,          COMMAND, 1, do_rm, 0)
    KEYWORD(rmdir,       COMMAND, 1, do_rmdir, 0)
    KEYWORD(seclabel,    OPTION,  0, 0, 0)
    KEYWORD(service,     SECTION, 0, 0, 0)
    KEYWORD(setcon,      COMMAND, 1, do_setcon, 0)
    KEYWORD(setenforce,  COMMAND, 1, do_setenforce, 0)
    KEYWORD(setenv,      OPTION,  2, 0, 0)
    KEYWORD(setkey,      COMMAND, 0, do_setkey, 0)
    KEYWORD(setprop,     COMMAND, 2, do_setprop, do_ext_setprop)
    KEYWORD(setrlimit,   COMMAND, 3, do_setrlimit, 0)
    KEYWORD(setsebool,   COMMAND, 1, do_setsebool, 0)
    KEYWORD(socket,      OPTION,  0, 0, 0)
    KEYWORD(start,       COMMAND, 1, do_start, 0)
    KEYWORD(stop,        COMMAND, 1, do_stop, 0)
    KEYWORD(trigger,     COMMAND, 1, do_trigger, 0)
    KEYWORD(symlink,     COMMAND, 1, do_symlink, 0)
    KEYWORD(sysclktz,    COMMAND, 1, do_sysclktz, 0)
    KEYWORD(user,        OPTION,  0, 0, 0)
    KEYWORD(wait,        COMMAND, 1, do_wait, 0)
    KEYWORD(write,       COMMAND, 2, do_write, 0)
    KEYWORD(setprop_from_sysfs,       COMMAND, 2, do_setprop_from_sysfs, 0)
    KEYWORD(copy,        COMMAND, 2, do_copy, 0)
    KEYWORD(chown,       COMMAND, 2, do_chown, 0)
    KEYWORD(chmod,       COMMAND, 2, do_chmod, 0)
    KEYWORD(loglevel,    COMMAND, 1, do_loglevel, 0)
    KEYWORD(load_persist_props,    COMMAND, 0, do_load_persist_props, 0)
    KEYWORD(ioprio,      OPTION,  0, 0, 0)
    KEYWORD(coldboot,    COMMAND, 1, do_builtin_coldboot, 0)
#ifdef __MAKE_KEYWORD_ENUM__
    KEYWORD_COUNT,
};
#undef __MAKE_KEYWORD_ENUM__
#undef KEYWORD
#endif

