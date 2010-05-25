/*
 * Wrappers around mutex/cond/thread functions
 *
 * Copyright Red Hat, Inc. 2009
 *
 * Author:
 *  Marcelo Tosatti <mtosatti@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include "qemu-thread.h"

static void error_exit(int err, const char *msg)
{
    fprintf(stderr, "qemu: %s: %s\n", msg, strerror(err));
    exit(1);
}

void qemu_mutex_init(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_init(&mutex->lock, NULL);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_lock(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_lock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

int qemu_mutex_trylock(QemuMutex *mutex)
{
    return pthread_mutex_trylock(&mutex->lock);
}

static void timespec_add_ms(struct timespec *ts, uint64_t msecs)
{
    ts->tv_sec = ts->tv_sec + (long)(msecs / 1000);
    ts->tv_nsec = (ts->tv_nsec + ((long)msecs % 1000) * 1000000);
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_nsec -= 1000000000;
        ts->tv_sec++;
    }
}

int qemu_mutex_timedlock(QemuMutex *mutex, uint64_t msecs)
{
    int err;
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    timespec_add_ms(&ts, msecs);

    err = pthread_mutex_timedlock(&mutex->lock, &ts);
    if (err && err != ETIMEDOUT)
        error_exit(err, __func__);
    return err;
}

void qemu_mutex_unlock(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_unlock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_init(QemuCond *cond)
{
    int err;

    err = pthread_cond_init(&cond->cond, NULL);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_signal(QemuCond *cond)
{
    int err;

    err = pthread_cond_signal(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_broadcast(QemuCond *cond)
{
    int err;

    err = pthread_cond_broadcast(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex)
{
    int err;

    err = pthread_cond_wait(&cond->cond, &mutex->lock);
    if (err)
        error_exit(err, __func__);
}

int qemu_cond_timedwait(QemuCond *cond, QemuMutex *mutex, uint64_t msecs)
{
    struct timespec ts;
    int err;

    clock_gettime(CLOCK_REALTIME, &ts);
    timespec_add_ms(&ts, msecs);

    err = pthread_cond_timedwait(&cond->cond, &mutex->lock, &ts);
    if (err && err != ETIMEDOUT)
        error_exit(err, __func__);
    return err;
}

void qemu_thread_create(QemuThread *thread,
                       void *(*start_routine)(void*),
                       void *arg)
{
    int err;

    /* Leave signal handling to the iothread.  */
    sigset_t set, oldset;

    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, &oldset);
    err = pthread_create(&thread->thread, NULL, start_routine, arg);
    if (err)
        error_exit(err, __func__);

    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
}

void qemu_thread_signal(QemuThread *thread, int sig)
{
    int err;

    err = pthread_kill(thread->thread, sig);
    if (err)
        error_exit(err, __func__);
}

void qemu_thread_self(QemuThread *thread)
{
    thread->thread = pthread_self();
}

int qemu_thread_equal(QemuThread *thread1, QemuThread *thread2)
{
   return pthread_equal(thread1->thread, thread2->thread);
}

#define futex(...)      syscall(__NR_futex, __VA_ARGS__)

void qemu_evcounter_init(QemuEvCounter *evcounter)
{
    evcounter->ctr = 0;
#ifndef CONFIG_FUTEX
    qemu_mutex_init(&evcounter->lock);
    qemu_cond_init(&evcounter->cond);
#endif
}

void qemu_evcounter_get(QemuEvCounterState *state, QemuEvCounter *evcounter)
{
#ifdef CONFIG_FUTEX
    __sync_fetch_and_add(&evcounter->waiters, 1);
#endif
    *state = evcounter->ctr;
}

void qemu_evcounter_wait(QemuEvCounterState *state, QemuEvCounter *evcounter)
{
#ifdef CONFIG_FUTEX
    if (*state == evcounter->ctr) {
        futex(&evcounter->ctr, FUTEX_WAIT, *state, NULL);
    }
#else
    qemu_mutex_lock(&evcounter->lock);
    while (*state == evcounter->ctr) {
        qemu_cond_wait(&evcounter->cond, &evcounter->lock);
    }
    qemu_mutex_unlock(&evcounter->lock);
#endif
    *state = evcounter->ctr;
}

void qemu_evcounter_timedwait(QemuEvCounterState *state,
			      QemuEvCounter *evcounter, uint64_t msecs)
{
#ifdef CONFIG_FUTEX
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    timespec_add_ms(&ts, msecs);

    if (*state == evcounter->ctr) {
        futex(&evcounter->ctr, FUTEX_WAIT, *state, &ts);
    }
#else
    qemu_mutex_lock(&evcounter->lock);
    while (*state == evcounter->ctr) {
        qemu_cond_timedwait(&evcounter->cond, &evcounter->lock, msecs);
    }
    qemu_mutex_unlock(&evcounter->lock);
#endif
    *state = evcounter->ctr;
}

void qemu_evcounter_signal(QemuEvCounter *evcounter)
{
#ifdef CONFIG_FUTEX
    /* This is written by one thread only, so there's no need to use locking
       primitives.  However, we rely on the implied memory barrier after
       it.  */
    __sync_fetch_and_add(evcounter->ctr, 1);
    if (evcounter->waiters != 0) {
        futex(&evcounter->ctr, FUTEX_WAKE, INT_MAX);
    }
#else
    qemu_mutex_lock(&evcounter->lock);
    evcounter->ctr++;
    qemu_cond_broadcast(&evcounter->cond);
    qemu_mutex_unlock(&evcounter->lock);
#endif
}

void qemu_evcounter_put(QemuEvCounter *evcounter)
{
#ifdef CONFIG_FUTEX
    __sync_fetch_and_add(&evcounter->waiters, -1);
#endif
}
