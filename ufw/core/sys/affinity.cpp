/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */
#include <ufw/core/sys/affinity.hpp>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#  include <sys/qos.h>
#endif

namespace ufw::core {

void pin_thread([[maybe_unused]] unsigned core) noexcept
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

} // namespace ufw::core
