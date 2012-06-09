/*
 * QEMU aio implementation
 *
 * Copyright IBM Corp., 2008
 * Copyright Red Hat Inc., 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Paolo Bonzini     <pbonzini@redhat.com>
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

/* This is a simple lock used to protect the aio_handlers list.  Specifically,
 * it's used to ensure that no callbacks are removed while we're walking and
 * dispatching callbacks.
 */
static int walking_handlers;

struct AioHandler {
    EventNotifier *e;
    EventNotifierHandler *io_notify;
    AioFlushEventNotifierHandler *io_flush;
    int deleted;
    QLIST_ENTRY(AioHandler) node;
};

void qemu_aio_set_event_notifier(EventNotifier *e,
                                 EventNotifierHandler *io_notify,
                                 AioFlushEventNotifierHandler *io_flush)
{
    AioHandler *node;

    QLIST_FOREACH(node, &aio_handlers, node) {
        if (node->e == e && !node->deleted) {
            break;
        }
    }

    /* Are we deleting the fd handler? */
    if (!io_notify) {
        if (node) {
            qemu_del_wait_object(event_notifier_get_handle(e),
                                 (WaitObjectFunc *) node->io_notify, e);

            /* If the lock is held, just mark the node as deleted */
            if (walking_handlers) {
                node->deleted = 1;
            } else {
                /* Otherwise, delete it for real.  We can't just mark it as
                 * deleted because deleted nodes are only cleaned up after
                 * releasing the walking_handlers lock.
                 */
                QLIST_REMOVE(node, node);
                g_free(node);
            }
        }
    } else {
        if (node == NULL) {
            /* Alloc and insert if it's not already there */
            node = g_malloc0(sizeof(AioHandler));
            node->e = e;
            QLIST_INSERT_HEAD(&aio_handlers, node, node);
        }
        /* Update handler with latest information */
        node->io_notify = io_notify;
        node->io_flush = io_flush;
        qemu_add_wait_object(event_notifier_get_handle(e),
                             (WaitObjectFunc *) io_notify, e);
    }
}

void qemu_aio_flush(void)
{
    while (qemu_aio_wait());
}

bool qemu_aio_wait(void)
{
    AioHandler *node;
    HANDLE events[MAXIMUM_WAIT_OBJECTS + 1];
    bool busy;
    int count;
    int ret;
    int timeout;

    /*
     * If there are callbacks left that have been queued, we need to call then.
     * Do not call select in this case, because it is possible that the caller
     * does not need a complete flush (as is the case for qemu_aio_wait loops).
     */
    if (qemu_bh_poll()) {
        return true;
    }

    walking_handlers = 1;

    /* fill fd sets */
    busy = false;
    count = 0;
    QLIST_FOREACH(node, &aio_handlers, node) {
        /* If there aren't pending AIO operations, don't invoke callbacks.
         * Otherwise, if there are no AIO requests, qemu_aio_wait() would
         * wait indefinitely.
         */
        if (node->io_flush) {
            if (node->io_flush(node->e) == 0) {
                continue;
            }
            busy = true;
        }
        if (!node->deleted && node->io_notify) {
            events[count++] = event_notifier_get_handle(node->e);
        }
    }

    walking_handlers = 0;

    /* No AIO operations?  Get us out of here */
    if (!busy) {
        return false;
    }

    /* wait until next event */
    timeout = INFINITE;
    for (;;) {
        ret = WaitForMultipleObjects(count, events, FALSE, timeout);
        if ((DWORD) (ret - WAIT_OBJECT_0) >= count) {
            break;
        }

        timeout = 0;

        /* if we have any signaled events, dispatch event */
        walking_handlers = 1;

        /* we have to walk very carefully in case
         * qemu_aio_set_fd_handler is called while we're walking */
        node = QLIST_FIRST(&aio_handlers);
        while (node) {
            AioHandler *tmp;

            if (!node->deleted &&
                event_notifier_get_handle(node->e) == events[ret - WAIT_OBJECT_0] &&
                node->io_notify) {
                node->io_notify(node->e);
            }

            tmp = node;
            node = QLIST_NEXT(node, node);

            if (tmp->deleted) {
                QLIST_REMOVE(tmp, node);
                g_free(tmp);
            }
        }

        walking_handlers = 0;
    }

    return true;
}
