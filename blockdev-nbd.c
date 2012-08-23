/*
 * QEMU host block devices
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "blockdev.h"
#include "hw/block-common.h"
#include "monitor.h"
#include "qerror.h"
#include "sysemu.h"
#include "qmp-commands.h"
#include "trace.h"
#include "nbd.h"
#include "qemu_socket.h"

static int server_fd = -1;

static void nbd_accept(void *opaque)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept(server_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd >= 0) {
        nbd_client_new(NULL, fd, NULL);
    }
}

void qmp_nbd_server_start(IPSocketAddress *addr, Error **errp)
{
    if (server_fd != -1) {
        /* TODO: error */
        return;
    }

    server_fd = inet_listen_opts(addr, 0, errp);
    if (server_fd != -1) {
        qemu_set_fd_handler2(server_fd, NULL, nbd_accept, NULL, NULL);
    }
}

/* Hook into the BlockDriverState notifiers to close the export when
 * the file is closed.
 */
typedef struct NBDCloseNotifier {
    Notifier n;
    NBDExport *exp;
    QTAILQ_ENTRY(NBDCloseNotifier) next;
} NBDCloseNotifier;

static QTAILQ_HEAD(, NBDCloseNotifier) close_notifiers =
    QTAILQ_HEAD_INITIALIZER(close_notifiers);

static void nbd_close_notifier_remove(NBDCloseNotifier *cn)
{
    notifier_remove(&cn->n);
    QTAILQ_REMOVE(&close_notifiers, cn, next);
    g_free(cn);
}

static void nbd_close_notifier(Notifier *n, void *data)
{
    NBDCloseNotifier *cn = DO_UPCAST(NBDCloseNotifier, n, n);

    nbd_export_close(cn->exp);
    nbd_close_notifier_remove(cn);
}

void qmp_nbd_server_add(const char *device, bool has_writable, bool writable,
                        Error **errp)
{
    BlockDriverState *bs;
    NBDExport *exp;
    NBDCloseNotifier *n;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (nbd_export_find(bdrv_get_device_name(bs))) {
        /* TODO: error */
        return;
    }

    exp = nbd_export_new(bs, 0, -1, writable ? 0 : NBD_FLAG_READ_ONLY);
    nbd_export_set_name(exp, device);

    n = g_malloc0(sizeof(NBDCloseNotifier));
    n->n.notify = nbd_close_notifier;
    n->exp = exp;
    bdrv_add_close_notifier(bs, &n->n);
    QTAILQ_INSERT_TAIL(&close_notifiers, n, next);
}

void qmp_nbd_server_stop(Error **errp)
{
    while (!QTAILQ_EMPTY(&close_notifiers)) {
        NBDCloseNotifier *cn = QTAILQ_FIRST(&close_notifiers);
        nbd_close_notifier_remove(cn);
    }

    nbd_export_close_all();
    qemu_set_fd_handler2(server_fd, NULL, NULL, NULL, NULL);
    close(server_fd);
    server_fd = -1;
}
