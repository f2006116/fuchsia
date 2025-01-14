// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <sched.h>
#include <threads.h>

#include <zxtest/zxtest.h>

namespace {

TEST(ConditionalVariableTest, BroadcastSignalWait) {
    struct CondThreadArgs {
        mtx_t mutex;
        cnd_t cond;
        int threads_woken;
        int threads_started;
        int threads_woke_first_barrier;
    };

    auto CondWaitHelper = [](void* arg) -> int {
        CondThreadArgs* args = static_cast<CondThreadArgs*>(arg);

        mtx_lock(&args->mutex);
        args->threads_started++;
        cnd_wait(&args->cond, &args->mutex);
        args->threads_woke_first_barrier++;
        cnd_wait(&args->cond, &args->mutex);
        args->threads_woken++;
        mtx_unlock(&args->mutex);
        return 0;
    };

    CondThreadArgs args = {};

    ASSERT_EQ(mtx_init(&args.mutex, mtx_plain), thrd_success);
    ASSERT_EQ(cnd_init(&args.cond), thrd_success);

    thrd_t thread1, thread2, thread3;

    ASSERT_EQ(thrd_create(&thread1, CondWaitHelper, &args), thrd_success);
    ASSERT_EQ(thrd_create(&thread2, CondWaitHelper, &args), thrd_success);
    ASSERT_EQ(thrd_create(&thread3, CondWaitHelper, &args), thrd_success);

    // Wait for all the threads to report that they've started.
    while (true) {
        mtx_lock(&args.mutex);
        int threads = args.threads_started;
        mtx_unlock(&args.mutex);
        if (threads == 3) {
            break;
        }
        sched_yield();
    }

    ASSERT_EQ(cnd_broadcast(&args.cond), thrd_success);

    // Wait for all the threads to report that they were woken.
    while (true) {
        mtx_lock(&args.mutex);
        int threads = args.threads_woke_first_barrier;
        mtx_unlock(&args.mutex);
        if (threads == 3) {
            break;
        }
        sched_yield();
    }

    for (int iteration = 0; iteration < 3; iteration++) {
        EXPECT_EQ(cnd_signal(&args.cond), thrd_success);

        // Wait for one thread to report that it was woken.
        while (true) {
            mtx_lock(&args.mutex);
            int threads = args.threads_woken;
            mtx_unlock(&args.mutex);
            if (threads == iteration + 1) {
                break;
            }
            sched_yield();
        }
    }

    constexpr int* kIgnoreReturn = nullptr;
    EXPECT_EQ(thrd_join(thread1, kIgnoreReturn), thrd_success);
    EXPECT_EQ(thrd_join(thread2, kIgnoreReturn), thrd_success);
    EXPECT_EQ(thrd_join(thread3, kIgnoreReturn), thrd_success);
}

void TimeAddNsec(struct timespec* ts, int nsec) {
    constexpr int kNsecPerSec = 1000000000;
    assert(nsec < kNsecPerSec);
    ts->tv_nsec += nsec;
    if (ts->tv_nsec > kNsecPerSec) {
        ts->tv_nsec -= kNsecPerSec;
        ts->tv_sec++;
    }
}

TEST(ConditionalVariableTest, ConditionalVariablesTimeout) {
    cnd_t cond = CND_INIT;
    mtx_t mutex = MTX_INIT;

    mtx_lock(&mutex);
    struct timespec delay;
    clock_gettime(CLOCK_REALTIME, &delay);
    TimeAddNsec(&delay, zx::msec(1).get());
    int result = cnd_timedwait(&cond, &mutex, &delay);
    mtx_unlock(&mutex);

    EXPECT_EQ(result, thrd_timedout, "Lock should have timedout");
}

} // namespace
