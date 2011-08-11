/*
 * QEMU buffered QEMUFile
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu-common.h"
#include "hw/hw.h"
#include "qemu-timer.h"
#include "qemu-char.h"
#include "buffered_file.h"
#include "migration.h"
#include "qemu-thread.h"

//#define DEBUG_BUFFERED_FILE

typedef struct QEMUFileBuffered
{
    BufferedPutFunc *put_buffer;
    BufferedPutReadyFunc *put_ready;
    BufferedCloseFunc *close;
    void *opaque;
    QEMUFile *file;
    int closed;
    size_t xfer_limit;
    uint8_t *buffer;
    size_t buffer_size;
    size_t buffer_offset;
    size_t buffer_capacity;
    QEMUBH *bh;
    QemuThread thread;
} QEMUFileBuffered;

#ifdef DEBUG_BUFFERED_FILE
#define DPRINTF(fmt, ...) \
    do { printf("buffered-file: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

static int buffered_put_buffer(void *opaque, const uint8_t *buf,
                               int64_t pos, int size)
{
    QEMUFileBuffered *s = opaque;

    DPRINTF("putting %d bytes at %" PRId64 "\n", size, pos);

    if (size == 0) {
        return 0;
    }

    if (size > (s->buffer_capacity - s->buffer_size)) {
        void *tmp;

        DPRINTF("increasing buffer capacity from %zu by %zu\n",
                s->buffer_capacity, size + 1024);

        s->buffer_capacity += size + 1024;

        tmp = g_realloc(s->buffer, s->buffer_capacity);
        if (tmp == NULL) {
            fprintf(stderr, "qemu file buffer expansion failed\n");
            exit(1);
        }

        s->buffer = tmp;
    }

    memcpy(s->buffer + s->buffer_size, buf, size);
    s->buffer_size += size;
    return size;
}

static size_t buffered_flush(QEMUFileBuffered *s)
{
    ssize_t ret;
    size_t len, total;
    int error;

    error = qemu_file_get_error(s->file);
    if (error != 0) {
        DPRINTF("flush when error, bailing: %s\n", strerror(-error));
        return error;
    }

    len = MIN(s->xfer_limit, s->buffer_size - s->buffer_offset);
    if (!len) {
        return 0;
    }

    total = 0;
    do {
        ret = s->put_buffer(s->opaque, s->buffer + s->buffer_offset, len);
        if (ret == 0) {
            ret = -EPIPE;
        }
        if (ret > 0) {
            total += ret;
            s->buffer_offset += ret;
        }
    } while (ret > 0);
    if (ret < 0) {
        qemu_file_set_error(s->file, ret);
    }
    if (s->buffer_offset == s->buffer_size) {
        s->buffer_offset = 0;
        s->buffer_size = 0;
    }
    return total;
}

static int buffered_close(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    QemuThread thread = s->thread;

    DPRINTF("closing\n");

    if (s->bh) {
        qemu_bh_delete(s->bh);
        s->bh = NULL;
    } else {
        s->closed = 1;
        qemu_mutex_unlock_iothread();
        qemu_thread_join(&thread);
        qemu_mutex_lock_iothread();
    }
    g_free(s);
    return 0;
}

/*
 * The meaning of the return values is:
 *   0: We can continue sending
 *   1: Time to stop
 *   negative: There has been an error
 */
static int buffered_rate_limit(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    int ret;

    ret = qemu_file_get_error(s->file);
    if (ret) {
        return ret;
    }

    return s->buffer_size - s->buffer_offset > s->xfer_limit;
}

static int64_t buffered_set_rate_limit(void *opaque, int64_t new_rate)
{
    QEMUFileBuffered *s = opaque;
    if (qemu_file_get_error(s->file)) {
        goto out;
    }
    if (new_rate > SIZE_MAX) {
        new_rate = SIZE_MAX;
    }

    s->xfer_limit = new_rate / 10;
    
out:
    return s->xfer_limit;
}

static int64_t buffered_get_rate_limit(void *opaque)
{
    QEMUFileBuffered *s = opaque;
  
    return s->xfer_limit;
}

static void *buffered_file_thread(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    size_t bytes_xfer = 0;
    int64_t expire_time = -1;
    struct timeval tv = { .tv_sec = 0 };

    while (!s->closed && qemu_file_get_error(s->file) == 0) {
        int64_t current_time = qemu_get_clock_ms(rt_clock);
        ssize_t ret;

        if (bytes_xfer >= s->xfer_limit) {
            tv.tv_usec = 1000 * (expire_time - current_time);
            select(0, NULL, NULL, NULL, &tv);
            expire_time = -1;
        }
        if (current_time >= expire_time) {
            bytes_xfer = 0;
            expire_time = current_time + 100;
        }

        ret = buffered_flush(s);
        if (ret >= 0) {
            bytes_xfer += ret;
        }

        if (s->buffer_size - s->buffer_offset < s->xfer_limit) {
            s->put_ready(s->opaque);
            qemu_fflush(s->file);
            if (s->buffer_size == 0) {
                break;
            }
        }
    }

    s->close(s->opaque);
    g_free(s->buffer);
    return NULL;
}

static void buffered_file_create_thread(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    qemu_thread_create(&s->thread, buffered_file_thread,
                       s, QEMU_THREAD_JOINABLE);
    qemu_bh_delete(s->bh);
    s->bh = NULL;
}

QEMUFile *qemu_fopen_ops_buffered(void *opaque,
                                  size_t bytes_per_sec,
                                  BufferedPutFunc *put_buffer,
                                  BufferedPutReadyFunc *put_ready,
                                  BufferedCloseFunc *close)
{
    QEMUFileBuffered *s;

    s = g_malloc0(sizeof(*s));

    s->opaque = opaque;
    s->xfer_limit = bytes_per_sec / 10;
    s->put_buffer = put_buffer;
    s->put_ready = put_ready;
    s->close = close;
    s->closed = 0;

    s->file = qemu_fopen_ops(s, buffered_put_buffer, NULL,
                             buffered_close, buffered_rate_limit,
                             buffered_set_rate_limit,
                             buffered_get_rate_limit);

    /* Use a bottom half to ensure the caller has time to store the
     * QEMUFile before s->put_ready is called for the first time.  */
    s->bh = qemu_bh_new(buffered_file_create_thread, s);
    qemu_bh_schedule(s->bh);

    return s->file;
}
