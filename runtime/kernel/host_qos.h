#pragma once
// Host thread priority on Apple. iPadOS / macOS schedule by quality-of-service class: a thread
// left at the default class may run on the efficiency cores, which are several times slower
// than the performance cores. Hot threads (the game's main thread, the GPU command processor)
// ask for USER_INTERACTIVE, the other guest threads for USER_INITIATED. Android: the hot threads
// ask for nice -8 (THREAD_PRIORITY_URGENT_DISPLAY, allowed for app threads), so the scheduler
// keeps them on the big cores. No-op elsewhere.
#ifdef __APPLE__
#   include <pthread/qos.h>
#elif defined(__ANDROID__)
#   include <sys/resource.h>
#   include <unistd.h>
#endif

namespace Kernel
{
    enum class HostQos { Interactive, Initiated };

    inline void SetHostThreadQos(HostQos qos)
    {
#ifdef __APPLE__
        pthread_set_qos_class_self_np(qos == HostQos::Interactive ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
#elif defined(__ANDROID__)
        setpriority(PRIO_PROCESS, gettid(), qos == HostQos::Interactive ? -8 : -2); // failure is harmless
#else
        (void)qos;
#endif
    }
}
