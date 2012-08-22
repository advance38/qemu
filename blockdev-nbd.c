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

static void nbd_server_start(QemuOpts *opts, Error **errp)
{
    if (server_fd != -1) {
        /* TODO: error */
        return;
    }

    server_fd = inet_listen_opts(opts, 0, errp);
    if (server_fd != -1) {
        qemu_set_fd_handler2(server_fd, NULL, nbd_accept, NULL, NULL);
    }
}

void qmp_nbd_server_start(IPSocketAddress *addr, Error **errp)
{
    QemuOpts *opts;

    opts = qemu_opts_create(&socket_opts, NULL, 0, NULL);
    qemu_opt_set(opts, "host", addr->host);
    qemu_opt_set(opts, "port", addr->port);

    addr->ipv4 |= !addr->has_ipv4;
    addr->ipv6 |= !addr->has_ipv6;
    if (!addr->ipv4 || !addr->ipv6) {
        qemu_opt_set_bool(opts, "ipv4", addr->ipv4);
        qemu_opt_set_bool(opts, "ipv6", addr->ipv6);
    }

    nbd_server_start(opts, errp);
    qemu_opts_del(opts);
}


void qmp_nbd_server_add(const char *device, bool has_writable, bool writable,
                        Error **errp)
{
    BlockDriverState *bs;
    NBDExport *exp;

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
}

void qmp_nbd_server_stop(Error **errp)
{
    nbd_export_close_all();
    qemu_set_fd_handler2(server_fd, NULL, NULL, NULL, NULL);
    close(server_fd);
    server_fd = -1;
}
