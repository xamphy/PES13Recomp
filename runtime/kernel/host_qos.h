#pragma once
// Host thread priority on Apple. iPadOS / macOS schedule by quality-of-service class: a thread
// left at the default class may run on the efficiency cores, which are several times slower
// than the performance cores. Hot threads (the game's main thread, the GPU command processor)
// ask for USER_INTERACTIVE, the other guest threads for USER_INITIATED. No-op elsewhere.
#ifdef __APPLE__
#   include <pthread/qos.h>
#endif

namespace Kernel
{
    enum class HostQos { Interactive, Initiated };

    inline void SetHostThreadQos(HostQos qos)
    {
#ifdef __APPLE__
        pthread_set_qos_class_self_np(qos == HostQos::Interactive ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
#else
        (void)qos;
#endif
    }
}
