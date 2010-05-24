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
 */

#include "qemu-common.h"
#include "block.h"
#include "qemu-queue.h"
#include "qemu_socket.h"

typedef struct AioHandler AioHandler;

/* The list of registered AIO handlers */
static QLIST_HEAD(, AioHandler) aio_handlers;

/* This is a simple lock used to protect the aio_handlers list.  Specifically,
 * it's used to ensure that no callbacks are removed while we're walking and
 * dispatching callbacks.
 */
static int walking_handlers;

struct AioHandler
{
    AioFlushHandler *io_flush;
    AioProcessQueue *io_process_queue;
    void *opaque;
    QLIST_ENTRY(AioHandler) node;
};

static AioHandler *find_aio_handler(void *opaque)
{
    AioHandler *node;

    QLIST_FOREACH(node, &aio_handlers, node) {
        if (node->opaque == opaque)
            return node;
    }

    return NULL;
}

int qemu_aio_set_handler(void *opaque,
			 AioFlushHandler *io_flush,
		      	 AioProcessQueue *io_process_queue)
{
    AioHandler *node;

    assert (io_flush || io_process_queue);

    node = find_aio_handler(opaque);
    if (node == NULL) {
        /* Alloc and insert if it's not already there */
        node = qemu_mallocz(sizeof(AioHandler));
        node->opaque = opaque;
        QLIST_INSERT_HEAD(&aio_handlers, node, node);
    }
    /* Update handler with latest information */
    node->io_flush = io_flush;
    node->io_process_queue = io_process_queue;
    return 0;
}

void qemu_aio_flush(void)
{
    AioHandler *node;
    int ret;

    do {
        ret = 0;

	/*
	 * If there are pending emulated aio start them now so flush
	 * will be able to return 1.
	 */
        qemu_aio_wait(NULL);

        QLIST_FOREACH(node, &aio_handlers, node) {
            if (node->io_flush) {
                ret |= node->io_flush(node->opaque);
            }
        }
    } while (qemu_bh_poll() || ret > 0);
}

int qemu_aio_process_queue(void)
{
    AioHandler *node;
    int ret = 0;

    walking_handlers = 1;

    /* we have to walk very carefully in case
     * qemu_aio_set_handler is called while we're walking */
    node = QLIST_FIRST(&aio_handlers);
    while (node) {
        AioHandler *tmp;

        if (node->io_process_queue) {
            if (node->io_process_queue(node->opaque)) {
                ret = 1;
            }
        }
        tmp = node;
        node = QLIST_NEXT(node, node);

        if (tmp->deleted) {
            QLIST_REMOVE(tmp, node);
            qemu_free(tmp);
        }
    }

    walking_handlers = 0;

    return ret;
}

void qemu_aio_wait(int *result)
{
    int async_ret = NOT_DONE;
    int ret;
    QemuEvCounterState state;

    if (!result) {
	/*
	 * Iterate once if result is NULL.
	 */
	result = &async_ret;
    }

    qemu_get_ioready(&state);
    for (; *result != NOT_DONE; async_ret = !NOT_DONE) {
        if (qemu_bh_poll())
            continue;

        /*
         * If there are callbacks left that have been queued, we need to call
         * them.  Return afterwards to avoid waiting needlessly in select().
         */
        if (qemu_aio_process_queue())
            continue;

        qemu_wait_ioready(&state);
	async_ret = !NOT_DONE;
    }
    qemu_put_ioready(&state);
}
