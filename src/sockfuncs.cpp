// socket functions for butt
//
// Copyright 2007-2008 by Daniel Noethen.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#ifdef _WIN32
 #include <winsock2.h>
// #define errno WSAGetLastError()
 #define EWOULDBLOCK WSAEWOULDBLOCK
#else
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <unistd.h>
 #include <netinet/in.h> //defines IPPROTO_TCP on BSD
 #include <netdb.h>
 #include <sys/select.h>
 #include <errno.h>
#endif

#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include "sockfuncs.h"

#ifdef _WIN32
 typedef int socklen_t;
#endif

#define MAX_TLS_SOCKS 16

static struct
{
    int fd;
    SSL *ssl;
} tls_socks[MAX_TLS_SOCKS];

static pthread_mutex_t tls_mut = PTHREAD_MUTEX_INITIALIZER;

static SSL *tls_lookup(int fd)
{
    SSL *ssl = NULL;

    pthread_mutex_lock(&tls_mut);
    for(int i = 0; i < MAX_TLS_SOCKS; i++)
    {
        if(tls_socks[i].ssl != NULL && tls_socks[i].fd == fd)
        {
            ssl = tls_socks[i].ssl;
            break;
        }
    }
    pthread_mutex_unlock(&tls_mut);

    return ssl;
}

static int tls_register(int fd, SSL *ssl)
{
    int ret = -1;

    pthread_mutex_lock(&tls_mut);
    for(int i = 0; i < MAX_TLS_SOCKS; i++)
    {
        if(tls_socks[i].ssl == NULL)
        {
            tls_socks[i].fd = fd;
            tls_socks[i].ssl = ssl;
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&tls_mut);

    return ret;
}

static SSL *tls_unregister(int fd)
{
    SSL *ssl = NULL;

    pthread_mutex_lock(&tls_mut);
    for(int i = 0; i < MAX_TLS_SOCKS; i++)
    {
        if(tls_socks[i].ssl != NULL && tls_socks[i].fd == fd)
        {
            ssl = tls_socks[i].ssl;
            tls_socks[i].ssl = NULL;
            tls_socks[i].fd = -1;
            break;
        }
    }
    pthread_mutex_unlock(&tls_mut);

    return ssl;
}

// Waits for the socket direction OpenSSL asked for after a non-blocking call.
// Returns 1 to retry, SOCK_TIMEOUT on timeout, 0 on a fatal TLS error.
static int tls_wait(int *s, SSL *ssl, int rc, int timout_ms)
{
    int mode;

    switch(SSL_get_error(ssl, rc))
    {
        case SSL_ERROR_WANT_READ:
            mode = READ;
            break;
        case SSL_ERROR_WANT_WRITE:
            mode = WRITE;
            break;
        default:
            return 0;
    }

    if(sock_select(s, timout_ms, mode) <= 0)
        return SOCK_TIMEOUT;

    return 1;
}

static int tls_handshake(int *s, const char *host, const char *ca_file)
{
    int rc;
    SSL_CTX *ctx;
    SSL *ssl;
    X509 *peer;
    X509_VERIFY_PARAM *param;

    ctx = SSL_CTX_new(TLS_client_method());
    if(ctx == NULL)
        return SOCK_ERR_TLS;

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    if(ca_file != NULL && ca_file[0] != '\0')
        rc = SSL_CTX_load_verify_locations(ctx, ca_file, NULL);
    else
        rc = SSL_CTX_set_default_verify_paths(ctx);

    if(rc != 1)
    {
        SSL_CTX_free(ctx);
        return SOCK_ERR_TLS;
    }

    ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    if(ssl == NULL)
        return SOCK_ERR_TLS;

    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    param = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

    // set1_ip_asc only accepts IP literals; anything else is a DNS name.
    if(X509_VERIFY_PARAM_set1_ip_asc(param, host) != 1)
    {
        if(X509_VERIFY_PARAM_set1_host(param, host, 0) != 1 ||
           SSL_set_tlsext_host_name(ssl, host) != 1)
        {
            SSL_free(ssl);
            return SOCK_ERR_TLS;
        }
    }

    if(SSL_set_fd(ssl, *s) != 1)
    {
        SSL_free(ssl);
        return SOCK_ERR_TLS;
    }

    for(;;)
    {
        ERR_clear_error();
        rc = SSL_connect(ssl);
        if(rc == 1)
            break;

        rc = tls_wait(s, ssl, rc, TLS_TIMEOUT);
        if(rc != 1)
        {
            SSL_free(ssl);
            return rc == SOCK_TIMEOUT ? SOCK_TIMEOUT : SOCK_ERR_TLS;
        }
    }

    // SSL_VERIFY_PEER does not fail the handshake when no certificate is sent.
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    peer = SSL_get1_peer_certificate(ssl);
#else
    peer = SSL_get_peer_certificate(ssl);
#endif
    if(peer == NULL || SSL_get_verify_result(ssl) != X509_V_OK)
    {
        X509_free(peer);
        SSL_free(ssl);
        return SOCK_ERR_TLS;
    }
    X509_free(peer);

    if(tls_register(*s, ssl) != 0)
    {
        SSL_free(ssl);
        return SOCK_ERR_TLS;
    }

    return 0;
}

int sock_connect_tls(const char *addr, short port, int timout_ms, const char *ca_file)
{
    int ret;
    int sock;

    sock = sock_connect(addr, port, timout_ms);
    if(sock < 0)
        return sock;

    ret = tls_handshake(&sock, addr, ca_file);
    if(ret != 0)
    {
        sock_close(&sock);
        return ret;
    }

    return sock;
}

static int tls_send(int *s, SSL *ssl, const char *buf, size_t len)
{
    int rc;
    size_t sent = 0;

    while(sent < len)
    {
        ERR_clear_error();
        rc = SSL_write(ssl, buf+sent, (int)(len-sent));
        if(rc > 0)
        {
            sent += rc;
            continue;
        }

        if(tls_wait(s, ssl, rc, 10000) != 1)
            return SOCK_TIMEOUT;
    }

    return (int)sent;
}

static ssize_t tls_recv(int *s, SSL *ssl, char *buf, int len, int timout_ms)
{
    int rc;
    int mode = READ;

    for(;;)
    {
        if(SSL_pending(ssl) == 0 && sock_select(s, timout_ms, mode) <= 0)
            return SOCK_TIMEOUT;

        ERR_clear_error();
        rc = SSL_read(ssl, buf, len);
        if(rc > 0)
            return rc;

        switch(SSL_get_error(ssl, rc))
        {
            case SSL_ERROR_WANT_READ:
                mode = READ;
                break;
            case SSL_ERROR_WANT_WRITE:
                mode = WRITE;
                break;
            case SSL_ERROR_ZERO_RETURN:
                return 0;
            default:
                return -1;
        }
    }
}


int sock_connect(const char *addr, short port, int timout_ms)
{

    int sock;
    struct hostent *host_ptr;
    struct sockaddr_in hostname;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2),&wsa);
#endif

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock == -1)
          return SOCK_ERR_CREATE;

    host_ptr = gethostbyname(addr);
    if(host_ptr == NULL)
    {
        host_ptr = gethostbyaddr(addr, (unsigned) strlen(addr), AF_INET);
        if(host_ptr == NULL)
        {
            sock_close(&sock);
            return SOCK_ERR_RESOLVE;
        }
    }

    hostname.sin_family = AF_INET;
    hostname.sin_port = htons(port);

    memcpy(&hostname.sin_addr, host_ptr->h_addr, host_ptr->h_length);

    
    sock_nonblock(&sock);

    connect(sock, (struct sockaddr*) &hostname, sizeof(hostname));

    if(sock_select(&sock, timout_ms, WRITE) <= 0)
    {
        sock_close(&sock);
        return SOCK_TIMEOUT;
    }

    if(!sock_isvalid(&sock))
    {
        sock_close(&sock);
        return SOCK_INVALID;
    }

    return sock;
}

int sock_setbufsize(int *s, int send_size, int recv_size)
{
    int ret;
    socklen_t len = sizeof(send_size);

    if(send_size > 0)
    {
        ret = setsockopt(*s, SOL_SOCKET, SO_SNDBUF, (char*)(&send_size), len);
        if(ret)
            return SOCK_ERR_SET_SBUF;
    }
    if(recv_size > 0)
    {
        ret = setsockopt(*s, SOL_SOCKET, SO_RCVBUF, (char*)(&recv_size), len);
        if(ret)
            return SOCK_ERR_SET_RBUF;
    }

    return 0;
}

int sock_send(int *s, const char *buf, size_t len, int timout_ms)
{
    ssize_t rc;
    int sent = 0;
    int error;
    SSL *ssl;

    if((ssl = tls_lookup(*s)) != NULL)
        return tls_send(s, ssl, buf, len);

    while(sent < len)
    {
        rc = sock_select(s, 10000, WRITE); //check if socket is connected
        
        if(rc == 0)
        {
            fflush(stdout);
            return SOCK_TIMEOUT;
        }

        if(rc == -1)
        {
            fflush(stdout);
            return SOCK_TIMEOUT;
        }

        if((rc = send(*s, buf+sent, len-sent, 0)) < 0)
        {
            
            error = errno;
            if(error != EWOULDBLOCK)
            {
                fflush(stdout);
                return SOCK_TIMEOUT;
            }
            rc = 0;
        }

        sent += rc;
    }
    return sent;
}

ssize_t sock_recv(int *s, char *buf, int len, int timout_ms)
{
    ssize_t rc;
    SSL *ssl;

    if((ssl = tls_lookup(*s)) != NULL)
        return tls_recv(s, ssl, buf, len, timout_ms);

    if(sock_select(s, timout_ms, READ) <= 0)
        return SOCK_TIMEOUT;

    rc = recv(*s, buf, len, 0);

    return rc;
}

int sock_nonblock(int *s)
{
#ifndef _WIN32
    long arg;

    arg = fcntl(*s, F_GETFL);
    arg |= O_NONBLOCK;

    return fcntl(*s, F_SETFL, arg);
#else
    unsigned long arg = 1;
    return ioctlsocket(*s, FIONBIO, &arg);
#endif
}

int sock_block(int *s)
{
#ifndef _WIN32
    long arg;

    arg = fcntl(*s, F_GETFL);
    arg &= ~O_NONBLOCK;

    return fcntl(*s, F_SETFL, arg);
#else
    unsigned long arg = 0;
    return ioctlsocket(*s, FIONBIO, &arg);
#endif
}

int sock_select(int *s, int timout_ms, int mode)
{
   struct timeval tv;
   fd_set fd_wr, fd_rd;

   FD_ZERO(&fd_wr);
   FD_ZERO(&fd_rd);
   FD_SET(*s, &fd_wr);
   FD_SET(*s, &fd_rd);

   tv.tv_sec = timout_ms/1000;
   tv.tv_usec = (timout_ms%1000)*1000;

   switch(mode)
   {
       case READ:
           return select(*s+1, &fd_rd, NULL, NULL, &tv);
           break;
       case WRITE:
           return select(*s+1, NULL, &fd_wr, NULL, &tv);
           break;
       case RW:
           return select(*s+1, &fd_rd, &fd_wr, NULL, &tv);
           break;
       default:
           return SOCK_NO_MODE;
   }
}

int sock_isvalid(int *s)
{
    int optval;
    socklen_t len = sizeof(optval);

    getsockopt(*s, SOL_SOCKET, SO_ERROR, (char*)(&optval), &len);

    if (optval)
        return 0;

    return 1;
}

void sock_close(int *s)
{
    SSL *ssl;

    if((ssl = tls_unregister(*s)) != NULL)
    {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }

#ifdef _WIN32
    closesocket(*s);
    WSACleanup();
#else
    close(*s);
#endif
}
