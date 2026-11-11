#include "itchbt/util/platform.hpp"

#include <sys/resource.h>

#if defined(__linux__)
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace itchbt {

std::string place_benchmark_thread(std::optional<unsigned> cpu) {
#if defined(__linux__)
    if (!cpu) {
        return "not pinned";
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(*cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return "pinning to cpu " + std::to_string(*cpu) + " failed";
    }
    return "pinned to cpu " + std::to_string(*cpu);
#elif defined(__APPLE__)
    static_cast<void>(cpu);
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0) {
        return "not pinned (QoS request failed)";
    }
    return "not pinned (macOS has no affinity API); QoS user-interactive, performance cores";
#else
    static_cast<void>(cpu);
    return "not pinned";
#endif
}

std::uint64_t peak_rss_bytes() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    const auto max_rss = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__APPLE__)
    return max_rss; // bytes on macOS
#else
    return max_rss * 1024; // kilobytes on Linux
#endif
}

PageFaults page_faults() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return {};
    }
    return {static_cast<std::uint64_t>(usage.ru_minflt),
            static_cast<std::uint64_t>(usage.ru_majflt)};
}

} // namespace itchbt
