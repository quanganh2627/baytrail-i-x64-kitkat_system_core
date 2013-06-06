/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the 
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#ifdef _WIN32
#include <winsock2.h>
typedef int socklen_t;
#define hstrerror(a) strerror(errno)
#else
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
extern int h_errno;
#endif

#include "tcp.h"
#include "fastboot.h"

#define FSTBOOT_PORT 1234
#define FSTBOOT_DFL_ADDR "192.168.42.1"

int tcp_write(void *userdata, const void *_data, int len)
{
    int len_tmp = len;
    int n;
    const char *_data_tmp = _data;
    tcp_handle *h = userdata;
    while (len_tmp > 0) {
        n = send(h->sockfd, _data_tmp, len_tmp, 0);
        if (n <= 0) {
            switch(errno) {
            case EAGAIN: case EINTR: continue;
            default:
                fprintf(stderr, "ERROR: Failed to send to network: %s\n",
                        strerror(errno));
                exit(1);
            }
        }
        len_tmp -= n;
        _data_tmp += n;
    }
    return len;
}

int tcp_read(void *userdata, void *_data, int len)
{
    int n, count;
    tcp_handle *h = userdata;
    count = 0;
    while (len > 0) {
        // This xfer chunking is to mirror usb_read() implementation:
        int xfer = (len > 16*1024) ? 16*1024 : len;
        n = recv(h->sockfd, _data, xfer, 0);
        if (n == 0) {
            fprintf(stderr, "ERROR: Failed to read network: "
                    "Unexpected end of file.");
            exit(1);
        } else if (n < 0) {
            switch(errno) {
            case EAGAIN: case EINTR: continue;
            default:
                fprintf(stderr, "ERROR: Failed to read network: %s",
                        strerror(errno));
                exit(1);
            }
        }
        count += n;
        len -= len;
        _data += n;

        // Replicate a bug from usb_read():
        if (n < xfer)
            break;
    }
    return count;
}

int tcp_close(void *userdata)
{
    tcp_handle *h = userdata;
    return close(h->sockfd);
}

#ifdef _WIN32
void exit_os(void) {
    WSACleanup();
}

void init_os(void) {
    WSADATA wsaData;
    int iResult;

    // Initialize Winsock
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed: %d\n", iResult);
        exit(1);
    }
    atexit(exit_os);
}
#endif

static int tcp_open_sock(char *host, struct sockaddr_in *serv_addr)
{
    int sockfd;
    struct hostent *server;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "ERROR: Can't open socket: %s\n", strerror(errno));
        return sockfd;
    }

    server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "ERROR: Can't find '%s': %s\n", host, hstrerror(h_errno));
        return -1;
    }

    memset(serv_addr, sizeof(*serv_addr), 0);
    serv_addr->sin_family = AF_INET;
    memcpy(&serv_addr->sin_addr.s_addr,
           server->h_addr,
           server->h_length);
    serv_addr->sin_port = htons(FSTBOOT_PORT);
    return sockfd;
}

/*
 * try to open a connection to 192.168.42.1:1234
 * in less than 100ms
 */
void tcp_list(void)
{
#ifndef _WIN32
    int sockfd;
    struct sockaddr_in serv_addr;
    int ret;
    fd_set wfds;
    struct timeval tv;

    sockfd = tcp_open_sock(FSTBOOT_DFL_ADDR, &serv_addr);
    if (sockfd < 0)
        return;

    if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0)
        return;

    if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        if (errno != EINPROGRESS)
            return;

        FD_ZERO(&wfds);
        FD_SET(sockfd, &wfds);

        /* Wait 100ms to connect. */
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        ret = select(sockfd + 1, NULL, &wfds, NULL, &tv);
        if (ret < 0)
            return;
        if (!FD_ISSET(sockfd, &wfds))
            return;
        if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
            return;
    }
    list_devices_callback(FSTBOOT_DFL_ADDR, NULL);
    close(sockfd);
#endif
}

tcp_handle *tcp_open(const char *host)
{
    int sockfd;
    struct sockaddr_in serv_addr;
    tcp_handle *tcp = 0;

#ifdef _WIN32
    init_os();
#endif

    sockfd = tcp_open_sock(FSTBOOT_DFL_ADDR, &serv_addr);
    if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
        fprintf(stderr, "ERROR: Unable to connect to %s: %s\n",
                host, strerror(errno));
        exit(1);
    }

    tcp = calloc(1, sizeof(tcp_handle));
    if (tcp != NULL) {
        tcp->sockfd = sockfd;
    }
    return tcp;
}

