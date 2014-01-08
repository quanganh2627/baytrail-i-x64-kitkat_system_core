#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <errno.h>
#include "tcp.h"


typedef const struct FHClassRec_*   FHClass;

typedef struct FHRec_*          FH;

typedef struct EventHookRec_*  EventHook;

typedef struct FHClassRec_
{
    void (*_fh_init) ( FH  f );
    int  (*_fh_close)( FH  f );
    int  (*_fh_read) ( FH  f, void*  buf, int  len );
    int  (*_fh_write)( FH  f, const void*  buf, int  len );

} FHClassRec;

typedef struct FHRec_
{
    FHClass    clazz;
    int        eof;
    union {
        HANDLE      handle;
        SOCKET      socket;
    } u;

    HANDLE    event;
    int       mask;

    char  name[32];

} FHRec;
#define  fh_handle  u.handle
#define  fh_socket  u.socket
#define  fh_pair    u.pair

#define  WIN32_FH_BASE    100

#define  WIN32_MAX_FHS    128

static  FHRec        _win32_fhs[ WIN32_MAX_FHS ];
static  int          _win32_fh_count;

static FH
_fh_from_int( int   fd )
{
    FH  f;

    fd -= WIN32_FH_BASE;

    if (fd < 0 || fd >= _win32_fh_count) {
        printf( "_fh_from_int: invalid fd %d\n", fd + WIN32_FH_BASE );
        errno = EBADF;
        return NULL;
    }

    f = &_win32_fhs[fd];

    if (!f->clazz) {
        printf( "_fh_from_int: invalid fd %d is not used\n", fd + WIN32_FH_BASE );
        errno = EBADF;
        return NULL;
    }

    return f;
}
static int
_fh_to_int( FH  f )
{
    if (f && f->clazz && f >= _win32_fhs && f < _win32_fhs + WIN32_MAX_FHS)
        return (int)(f - _win32_fhs) + WIN32_FH_BASE;

    return -1;
}

static FH
_fh_alloc( FHClass  clazz )
{
    int  nn;
    FH   f = NULL;

    if (_win32_fh_count < WIN32_MAX_FHS) {
        f = &_win32_fhs[ _win32_fh_count++ ];
        goto Exit;
    }

    for (nn = 0; nn < WIN32_MAX_FHS; nn++) {
        if ( _win32_fhs[nn].clazz == NULL) {
            f = &_win32_fhs[nn];
            goto Exit;
        }
    }
    printf( "_fh_alloc: no more free file descriptors\n" );
Exit:
    if (f) {
        f->clazz = clazz;
        f->eof   = 0;
        clazz->_fh_init( f );
    }
    return f;
}

static int
_fh_close( FH   f )
{
    if ( f->clazz ) {
        f->clazz->_fh_close( f );
        f->eof  = 0;
        f->clazz = NULL;
    }
    return 0;
}

/* forward definitions */
static const FHClassRec   _fh_file_class;
static const FHClassRec   _fh_socket_class;

static void
_socket_set_errno( void )
{
    switch (WSAGetLastError()) {
    case 0:              errno = 0; break;
    case WSAEWOULDBLOCK: errno = EAGAIN; break;
    case WSAEINTR:       errno = EINTR; break;
    default:
        printf( "_socket_set_errno: unhandled value %d\n", WSAGetLastError() );
        errno = EINVAL;
    }
}

static void
_fh_socket_init( FH  f )
{
    f->fh_socket = INVALID_SOCKET;
    f->event     = WSACreateEvent();
    f->mask      = 0;
}

static int
_fh_socket_close( FH  f )
{
    /* gently tell any peer that we're closing the socket */
    shutdown( f->fh_socket, SD_BOTH );
    closesocket( f->fh_socket );
    f->fh_socket = INVALID_SOCKET;
    CloseHandle( f->event );
    f->mask = 0;
    return 0;
}

static int
_fh_socket_read( FH  f, void*  buf, int  len )
{
    int n, count = 0;
    unsigned char *data = buf;
    while (len > 0) {
        // This xfer chunking is to mirror usb_read() implementation:
        int xfer = (len > 16*1024) ? 16*1024 : len;
        n = recv(f->fh_socket, (void*)data, xfer, 0);
        if (n == 0) {
            printf( "ERROR: Failed to read network: Unexpected end of file." );
            _socket_set_errno();
            exit(1);
        } else if (n < 0) {
            switch(errno) {
            case EAGAIN: case EINTR: continue;
            default:
                _socket_set_errno();
                exit(1);
            }
        }
        count += n;
        len -= len;
        data += n;

        // Replicate a bug from usb_read():
        if (n < xfer)
            break;
    }
    return count;

}

static int
_fh_socket_write( FH  f, const void*  buf, int  len )
{
    int len_tmp = len;
    int n;
    const char *_data_tmp = buf;
    while (len_tmp > 0) {
        n = send(f->fh_socket, _data_tmp, len_tmp, 0);
        if (n <= 0) {
            switch(errno) {
            case EAGAIN: case EINTR: continue;
            default:
               _socket_set_errno();
               exit(1);
            }
        }
        len_tmp -= n;
        _data_tmp += n;
    }
    return len;
}

static const FHClassRec  _fh_socket_class =
{
    _fh_socket_init,
    _fh_socket_close,
    _fh_socket_read,
    _fh_socket_write,
};

static int  _winsock_init;

static void
_cleanup_winsock( void )
{
    WSACleanup();
}

static void
_init_winsock( void )
{
    if (!_winsock_init) {
        WSADATA  wsaData;
        int      rc = WSAStartup( MAKEWORD(2,2), &wsaData);
        if (rc != 0) {
            printf( "fastboot: could not initialize Winsock\n" );
            exit(-1);
        }
        atexit( _cleanup_winsock );
        _winsock_init = 1;
    }
}

#define LISTEN_BACKLOG 4
#define DEFAULT_PORT 1234

tcp_handle *tcp_open(const char *host)
{
    FH  f = _fh_alloc( &_fh_socket_class );
    struct sockaddr_in addr;
    SOCKET  s;
    int n;
    int ret;
    unsigned tid = 0;
    tcp_handle *tcp = 0;
    struct hostent *server;
    if (!f){
        printf( "Error: Cannot initialize fh" );
        exit(-1);
    }

    if (!_winsock_init)
        _init_winsock();

    server = gethostbyname(host);
    if (server == NULL) {
        printf( "ERROR: Can't find '%s'\n", host );
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);
    memcpy(&addr.sin_addr.s_addr,
           server->h_addr,
           server->h_length);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if(s == INVALID_SOCKET) {
        _fh_close(f);
        printf( "Error: Cannot initialize socket" );
        exit(-1);
    }

    f->fh_socket = s;
    if(connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        printf( "tcp_open: could not connect to tcp port\n" );
        _fh_close(f);
        exit(-1);
    }
    tcp = calloc(1, sizeof(tcp_handle));
    if (tcp == NULL) {
        printf( "ERROR: Unable to allocate memory: %s\n",strerror(errno) );
        exit(-1);
    }
    tcp->sockfd = _fh_to_int(f);

    return tcp;
}

int tcp_close(void *userdata)
{
    tcp_handle *h = (tcp_handle*) userdata;
    FH serverfh = _fh_from_int(h->sockfd);
    if ( !serverfh || serverfh->clazz != &_fh_socket_class ) {
        printf( "fastoot tcp_close: invalid socket %d\n", h->sockfd );
        return -1;
    }
    return _fh_close(serverfh);
}

int tcp_write(void *userdata, const void *_data, int len)
{
    tcp_handle *h = (tcp_handle*) userdata;
    FH serverfh = _fh_from_int(h->sockfd);
    if ( !serverfh || serverfh->clazz != &_fh_socket_class ) {
        printf( "fastoot tcp_write: invalid socket %d\n",  h->sockfd );
        return -1;
    }

    return _fh_socket_write( serverfh, _data, len);
}

int tcp_read(void *userdata, void *_data, int len)
{

    tcp_handle *h = (tcp_handle*) userdata;
    FH serverfh = _fh_from_int(h->sockfd);
    if ( !serverfh || serverfh->clazz != &_fh_socket_class ) {
        printf( "fastoot tcp_read: invalid socket %d\n",  h->sockfd );
        return -1;
    }

    return _fh_socket_read( serverfh, _data, len);
}
