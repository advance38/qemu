/*
 * QEMU aio implementation
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
#include "block.h"
#include "qemu-queue.h"
#include "qemu_socket.h"

typedef struct AioHandler AioHandler;

/* The list of registered AIO handlers */
static QLIST_HEAD(, AioHandler) aio_handlers;

struct AioHandler
{
    int fd;
    IOHandler *io_read;
    IOHandler *io_write;
    AioFlushHandler *io_flush;
    void *opaque;
    QLIST_ENTRY(AioHandler) node;
};

static AioHandler *find_aio_handler(int fd)
{
    AioHandler *node;

    QLIST_FOREACH(node, &aio_handlers, node) {
        if (node->fd == fd) {
            return node;
	}
    }

    return NULL;
}

int qemu_aio_set_fd_handler(int fd,
                            IOHandler *io_read,
                            IOHandler *io_write,
                            AioFlushHandler *io_flush,
                            void *opaque)
{
    AioHandler *node;

    node = find_aio_handler(fd);

    /* Are we deleting the fd handler? */
    if (!io_read && !io_write) {
        if (node) {
            QLIST_REMOVE(node, node);
            g_free(node);
        }
    } else {
        if (node == NULL) {
            /* Alloc and insert if it's not already there */
            node = g_malloc0(sizeof(AioHandler));
            node->fd = fd;
            QLIST_INSERT_HEAD(&aio_handlers, node, node);
        }
        /* Update handler with latest information */
        node->io_read = io_read;
        node->io_write = io_write;
        node->io_flush = io_flush;
        node->opaque = opaque;
    }

    qemu_set_fd_handler2(fd, NULL, io_read, io_write, opaque);

    return 0;
}

static bool qemu_aio_pending(void)
{
    AioHandler *node;

    QLIST_FOREACH(node, &aio_handlers, node) {
        if (node->deleted) {
            continue;
        }
        if (node->io_flush && node->io_flush(node->opaque)) {
            return 1;
        }
    }
    return 0;
}

void qemu_aio_flush(void)
{
    int ret;
    bool first = true;
    do {
        ret = main_loop_wait(first);
        first = false;
    } while (ret || qemu_aio_pending());
}

void qemu_aio_wait(void)
{
    /* If there aren't pending AIO operations, no need to do anything.
     * Otherwise, if there are no AIO requests, qemu_aio_wait() would
     * wait indefinitely.
     */
    if (qemu_aio_pending()) {
        main_loop_wait(false);
    }
}
