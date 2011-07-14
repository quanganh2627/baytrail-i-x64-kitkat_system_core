#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/klog.h>
#include <string.h>

#define KLOG_BUF_MAX_SHIFT  20    /* Currently, CONFIG_LOG_BUF_SHIFT from our kernel is 19 */
#define KLOG_BUF_MAX_LEN    (1 << KLOG_BUF_MAX_SHIFT)

int dmesg_main(int argc, char **argv)
{
    char *buffer, *p;
    ssize_t ret;
    int n, op, len;

    len = klogctl(KLOG_WRITE, NULL, 0);    /* read ring buffer size */
    if (len > KLOG_BUF_MAX_LEN)
        len = KLOG_BUF_MAX_LEN;
    buffer = malloc(len + 1);
    if (!buffer) {
        perror("klogctl");
        return EXIT_FAILURE;
    }

    if ((argc == 2) && (!strcmp(argv[1], "-c"))) {
        op = KLOG_READ_CLEAR;
    } else {
        op = KLOG_READ_ALL;
    }

    n = klogctl(op, buffer, len);
    if (n < 0) {
        perror("klogctl");
        free(buffer);
        return EXIT_FAILURE;
    }
    buffer[n] = '\0';

    p = buffer;
    while ((ret = write(STDOUT_FILENO, p, n))) {
        if (ret == -1) {
            if (errno == EINTR)
                continue;
            perror("write");
            free(buffer);
            return EXIT_FAILURE;
        }
        p += ret;
        n -= ret;
    }

    free(buffer);

    return 0;
}
