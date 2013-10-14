#ifndef UFDISK_H
#define UFDISK_H

#ifdef __cplusplus
extern "C" {
#endif

int ufdisk_need_create_partition(void);
int ufdisk_create_partition(void);
void ufdisk_umount_all(void);

#ifdef __cplusplus
}
#endif

#endif
