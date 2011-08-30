#ifndef __QEMU_THREAD_WIN32_H
#define __QEMU_THREAD_WIN32_H 1
#include "windows.h"

struct QemuMutex {
    HANDLE sema;
    LONG count;
};

struct QemuRWMutex {
    CRITICAL_SECTION readerCountLock;
    CRITICAL_SECTION writerLock;
    HANDLE noReaders;
    HANDLE writer;
    int readerCount;
};

struct QemuCond {
    QemuMutex *mutex;
    LONG waiters, target;
    HANDLE sema;
    HANDLE continue_event;
};

struct QemuThread {
    HANDLE thread;
    void *ret;
};

#endif
