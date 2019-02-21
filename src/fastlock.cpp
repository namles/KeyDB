/* 
 * Copyright (c) 2019, John Sully <john at eqalpha dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fastlock.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sched.h>
#include <atomic>

/****************************************************
 *
 *      Implementation of a fair spinlock.  To promote fairness we
 *      use a ticket lock instead of a raw spinlock
 * 
 ****************************************************/

static_assert(sizeof(pid_t) <= sizeof(fastlock::m_pidOwner), "fastlock::m_pidOwner not large enough");

static pid_t gettid()
{
    static thread_local int pidCache = -1;
    if (pidCache == -1)
        pidCache = syscall(SYS_gettid);
    return pidCache;
}

extern "C" void fastlock_init(struct fastlock *lock)
{
    lock->m_active = 0;
    lock->m_avail = 0;
    lock->m_depth = 0;
}

extern "C" void fastlock_lock(struct fastlock *lock)
{
    if ((int)__atomic_load_4(&lock->m_pidOwner, __ATOMIC_ACQUIRE) == gettid())
    {
        ++lock->m_depth;
        return;
    }

    unsigned myticket = __atomic_fetch_add(&lock->m_avail, 1, __ATOMIC_ACQ_REL);

    if (__atomic_load_4(&lock->m_active, __ATOMIC_ACQUIRE) != myticket)
    {
        int cloops = 1;
        while (__atomic_load_4(&lock->m_active, __ATOMIC_ACQUIRE) != myticket)
        {
            if ((++cloops % 1024*1024) == 0)
                sched_yield();
        }
    }

    lock->m_depth = 1;
    __atomic_store_4(&lock->m_pidOwner, gettid(), __ATOMIC_RELEASE);
    __sync_synchronize();
}

extern "C" void fastlock_unlock(struct fastlock *lock)
{
    --lock->m_depth;
    if (lock->m_depth == 0)
    {
        lock->m_pidOwner = -1;
        __sync_synchronize();
        __atomic_fetch_add(&lock->m_active, 1, __ATOMIC_ACQ_REL);
    }
}

extern "C" void fastlock_free(struct fastlock *lock)
{
    // NOP
    (void)lock;
}


bool fastlock::fOwnLock()
{
    return gettid() == m_pidOwner;
}