#ifndef __QEMU_THREAD_H
#define __QEMU_THREAD_H 1

#include <inttypes.h>

typedef struct QemuMutex QemuMutex;
typedef struct QemuRWMutex QemuRWMutex;
typedef struct QemuCond QemuCond;
typedef struct QemuThread QemuThread;

#ifdef _WIN32
#include "qemu-thread-win32.h"
#else
#include "qemu-thread-posix.h"
#endif

void qemu_mutex_init(QemuMutex *mutex);
void qemu_mutex_destroy(QemuMutex *mutex);
void qemu_mutex_lock(QemuMutex *mutex);
int qemu_mutex_trylock(QemuMutex *mutex);
void qemu_mutex_unlock(QemuMutex *mutex);

void qemu_rwmutex_init(QemuRWMutex *mutex);
void qemu_rwmutex_rdlock(QemuRWMutex *mutex);
void qemu_rwmutex_wrlock(QemuRWMutex *mutex);
void qemu_rwmutex_unlock(QemuRWMutex *mutex);

void qemu_cond_init(QemuCond *cond, QemuMutex *mutex);
void qemu_cond_destroy(QemuCond *cond);
void qemu_cond_signal(QemuCond *cond);
void qemu_cond_broadcast(QemuCond *cond);
void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex);

void qemu_thread_create(QemuThread *thread,
                       void *(*start_routine)(void*),
                       void *arg);
void qemu_thread_get_self(QemuThread *thread);
int qemu_thread_is_self(QemuThread *thread);
void qemu_thread_exit(void *retval);
void *qemu_thread_join(QemuThread *thread);

#endif
