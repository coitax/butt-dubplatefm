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

#ifndef SOCKFUNCS_H
#define SOCKFUNCS_H

enum {
    READ = 0,
    WRITE = 1,
    RW = 2
};

enum {
    SOCK_ERR_CREATE = -1,
    SOCK_ERR_RESOLVE = -2,
    SOCK_TIMEOUT = -3,
    SOCK_INVALID = -4,
    SOCK_NO_MODE = -5,
    SOCK_ERR_SET_SBUF = -6,
    SOCK_ERR_SET_RBUF = -7,
    SOCK_ERR_TLS = -8
};

enum {
    CONN_TIMEOUT = 1000,
    SEND_TIMEOUT = 3000,
    RECV_TIMEOUT = 1000,
    TLS_TIMEOUT = 5000
};


int sock_connect(const char *addr, short port, int timout_ms);
// Connects and completes a TLS handshake whose certificate chain verifies
// against ca_file (or the system trust store when ca_file is NULL/empty) and
// matches addr. Returns SOCK_ERR_TLS without leaving a socket open otherwise.
int sock_connect_tls(const char *addr, short port, int timout_ms, const char *ca_file);
int sock_setbufsize(int *s, int send_size, int recv_size);
int sock_isdisconnected(int *s);
int sock_send(int *s, const char *buf, size_t len, int timout_ms);
ssize_t sock_recv(int *s, char *buf, int len, int timout_ms);
int sock_select(int *s, int timout_ms, int mode);
int sock_nonblock(int *s);
int sock_block(int *s);
int sock_isvalid(int *s);
void sock_close(int *s);
void sock_fdinit(int *s);
void sock_fdclr(int *s);
void sock_fdzero(void);


#endif

