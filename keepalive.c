/*

  Copyright (c) 2016 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "iov.h"
#include "msock.h"
#include "dsock.h"
#include "utils.h"

dsock_unique_id(keepalive_type);

static void *keepalive_hquery(struct hvfs *hvfs, const void *type);
static void keepalive_hclose(struct hvfs *hvfs);
static int keepalive_msendv(struct msock_vfs *mvfs,
    const struct iovec *iov, size_t iovlen, int64_t deadline);
static ssize_t keepalive_mrecvv(struct msock_vfs *mvfs,
    const struct iovec *iov, size_t iovlen, int64_t deadline);
static coroutine void keepalive_sender(int s, int64_t send_interval,
    int sendch, int ackch);

#define KEEPALIVE_SOCK_PEERDONE 1

struct keepalive_sock {
    struct hvfs hvfs;
    struct msock_vfs mvfs;
    int s;
    int64_t send_interval;
    int64_t recv_interval;
    int sendch;
    int ackch;
    int sender;
    int64_t last_recv;
    int flags;
};

struct keepalive_vec {
    const struct iovec *iov;
    size_t iovlen;
};

int keepalive_start(int s, int64_t send_interval, int64_t recv_interval) {
    int rc;
    int err;
    /* Check whether underlying socket is a bytestream. */
    if(dsock_slow(!hquery(s, msock_type))) {err = errno; goto error1;}
    /* Create the object. */
    struct keepalive_sock *obj = malloc(sizeof(struct keepalive_sock));
    if(dsock_slow(!obj)) {err = ENOMEM; goto error1;}
    obj->hvfs.query = keepalive_hquery;
    obj->hvfs.close = keepalive_hclose;
    obj->mvfs.msendv = keepalive_msendv;
    obj->mvfs.mrecvv = keepalive_mrecvv;
    obj->s = s;
    obj->send_interval = send_interval;
    obj->recv_interval = recv_interval;
    obj->sendch = -1;
    obj->ackch = -1;
    obj->sender = -1;
    if(send_interval >= 0) {
        obj->sendch = channel(sizeof(struct keepalive_vec), 0);
        if(dsock_slow(obj->sendch < 0)) {err = errno; goto error2;}
        obj->ackch = channel(sizeof(int), 0);
        if(dsock_slow(obj->ackch < 0)) {err = errno; goto error3;}
        obj->sender = go(keepalive_sender(s, send_interval,
            obj->sendch, obj->ackch));
        if(dsock_slow(obj->sender < 0)) {err = errno; goto error4;}
    }
    obj->last_recv = now();
    obj->flags = 0;
    /* Create the handle. */
    int h = hcreate(&obj->hvfs);
    if(dsock_slow(h < 0)) {err = errno; goto error5;}
    return h;
error5:
    if(obj->sender >= 0) {
        rc = hclose(obj->sender);
        dsock_assert(rc == 0);
    }
error4:
    if(obj->ackch >= 0) {
        rc = hclose(obj->ackch);
        dsock_assert(rc == 0);
    }
error3:
    if(obj->sendch >= 0) {
        rc = hclose(obj->sendch);
        dsock_assert(rc == 0);
    }
error2:
    free(obj);
error1:
    errno = err;
    return -1;
}

static int keepalive_free(struct keepalive_sock *obj) {
    if(obj->send_interval >= 0) {
        int rc = hclose(obj->sender);
        dsock_assert(rc == 0);
        rc = hclose(obj->ackch);
        dsock_assert(rc == 0);
        rc = hclose(obj->sendch);
        dsock_assert(rc == 0);
    }
    int u = obj->s;
    free(obj);
    return u;
}

int keepalive_stop(int s, int64_t deadline) {
    int err;
    struct keepalive_sock *obj = hquery(s, keepalive_type);
    if(dsock_slow(!obj)) return -1;
    int rc = msend(obj->s, "T", 1, deadline);
    if(dsock_slow(rc <  0)) {err = errno; goto error;}
    while(!obj->flags & KEEPALIVE_SOCK_PEERDONE) {
        int rc = keepalive_mrecvv(&obj->mvfs, NULL, 0, deadline);
        if(rc < 0 && errno == EPIPE) break;
        if(dsock_slow(rc < 0 && errno != EMSGSIZE)) {err = errno; goto error;}
    }
    return keepalive_free(obj);
error:
    rc = hclose(s);
    if(dsock_slow(rc < 0)) return -1;
    errno = err;
    return -1;
}

static int keepalive_msendv(struct msock_vfs *mvfs,
      const struct iovec *iov, size_t iovlen, int64_t deadline) {
    struct keepalive_sock *obj = dsock_cont(mvfs, struct keepalive_sock, mvfs);
    /* Send is done in a worker coroutine. */
    struct keepalive_vec vec = {iov, iovlen};
    int rc = chsend(obj->sendch, &vec, sizeof(vec), deadline);
    if(dsock_slow(rc < 0)) return -1;
    /* Wait till worker is done. */
    int err;
    rc = chrecv(obj->ackch, &err, sizeof(err), deadline);
    if(dsock_slow(rc < 0)) return -1;
    if(dsock_slow(err < 0)) {errno = err; return -1;}
    return 0;
}

static coroutine void keepalive_sender(int s, int64_t send_interval,
      int sendch, int ackch) {
    /* Last time something was sent. */
    int64_t last = now();
    while(1) {
        /* Get data to send from the user coroutine. */
        struct keepalive_vec vec;
        int rc = chrecv(sendch, &vec, sizeof(vec), last + send_interval);
        if(dsock_slow(rc < 0 && errno == ECANCELED)) return;
        if(dsock_slow(rc < 0 && errno == ETIMEDOUT)) {
            /* Send a keepalive. */
            rc = msend(s, "K", 1, -1);
            if(dsock_slow(rc < 0 && errno == ECANCELED)) return;
            if(dsock_slow(rc < 0 && errno == ECONNRESET)) return;
            dsock_assert(rc == 0);
            last = now();
            continue;
        }
        dsock_assert(rc == 0);
        struct iovec iov[vec.iovlen + 1];
        uint8_t c = 'D';
        iov[0].iov_base = &c;
        iov[0].iov_len = 1;
        iov_copy(iov + 1, vec.iov, vec.iovlen);
        rc = msendv(s, iov, vec.iovlen + 1, -1);
        if(dsock_slow(rc < 0 && errno == ECANCELED)) return;
        /* Pass the error to the user. */
        if(dsock_slow(rc < 0)) {
            int err = errno;
            rc = chsend(ackch, &err, sizeof(err), -1);
            if(dsock_slow(rc < 0 && errno == ECANCELED)) return;
            dsock_assert(rc == 0);
            return;
        }
        last = now();
        int err = 0;
        rc = chsend(ackch, &err, sizeof(err), -1);
        if(dsock_slow(rc < 0 && errno == ECANCELED)) return;
        dsock_assert(rc == 0);
    }
}

static ssize_t keepalive_mrecvv(struct msock_vfs *mvfs,
      const struct iovec *iov, size_t iovlen, int64_t deadline) {
    struct keepalive_sock *obj = dsock_cont(mvfs, struct keepalive_sock, mvfs);
    if(dsock_slow(obj->flags & KEEPALIVE_SOCK_PEERDONE)) {
        errno = EPIPE; return -1;}
    /* If receive mode is off, just forward the call. */
    if(obj->recv_interval < 0) return mrecvv(obj->s, iov, iovlen, deadline);
    /* Compute the deadline. Take keepalive interval into consideration. */
retry:;
    int64_t dd = obj->last_recv + obj->recv_interval;
    int fail_on_deadline = 1;
    if(deadline < dd) {
       dd = deadline;
       fail_on_deadline = 0;
    }
    struct iovec vec[iovlen + 1];
    uint8_t c;
    vec[0].iov_base = &c;
    vec[0].iov_len = 1;
    iov_copy(vec + 1, iov, iovlen);
    ssize_t sz = mrecvv(obj->s, vec, iovlen + 1, dd);
    if(dsock_slow(fail_on_deadline && sz < 0 && errno == ETIMEDOUT)) {
        errno = ECONNRESET; return -1;}
    if(dsock_slow(sz < 0)) return -1;
    obj->last_recv = now();
    /* TODO: Broken protocol. */
    dsock_assert(sz != 0);
    /* Filter out keepalive messages. */
    if(dsock_slow(c == 'K')) goto retry;
    /* Protocol termination. */
    if(dsock_slow(c == 'T')) {
        obj->flags |= KEEPALIVE_SOCK_PEERDONE;
        errno = EPIPE;
        return -1;
    }
    return sz - 1;
}

static void *keepalive_hquery(struct hvfs *hvfs, const void *type) {
    struct keepalive_sock *obj = (struct keepalive_sock*)hvfs;
    if(type == msock_type) return &obj->mvfs;
    if(type == keepalive_type) return obj;
    errno = ENOTSUP;
    return NULL;
}

static void keepalive_hclose(struct hvfs *hvfs) {
    struct keepalive_sock *obj = (struct keepalive_sock*)hvfs;
    int u = keepalive_free(obj);
    int rc = hclose(u);
    dsock_assert(rc == 0);
}

