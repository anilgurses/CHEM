#pragma once

#include <numa.h>
#include <numaif.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <vector>

namespace chem {
namespace numa {

// Returns true if libnuma is available on this system.
inline bool available() { return numa_available() >= 0; }

// Returns the number of configured NUMA nodes.
inline int node_count() { return numa_num_configured_nodes(); }

// Returns the NUMA node of the calling thread's current CPU.
inline int current_node() {
    int cpu = sched_getcpu();
    if (cpu < 0) return 0;
    return numa_node_of_cpu(cpu);
}

// Picks the NUMA node with the most free memory.
inline int pick_least_loaded_node() {
    int best_node = 0;
    long long best_free = 0;

    int n = numa_num_configured_nodes();
    for (int i = 0; i < n; i++) {
        long long total = 0;
        long long free_mem = numa_node_size64(i, &total);
        if (free_mem > best_free) {
            best_free = free_mem;
            best_node = i;
        }
    }
    return best_node;
}

// Sets the preferred NUMA node for memory allocation.
// Pass -1 to reset to default (local) policy.
inline void prefer_node(int node) { numa_set_preferred(node); }

// Binds the current process to a NUMA node's CPUs and sets memory preference.
inline void bind_process_to_node(int node) {
    struct bitmask* cpumask = numa_allocate_cpumask();
    if (numa_node_to_cpus(node, cpumask) == 0) {
        numa_sched_setaffinity(0, cpumask);
    }
    numa_bitmask_free(cpumask);
    numa_set_preferred(node);
}

// Binds a specific thread to a NUMA node's CPUs.
inline void bind_thread_to_node(pthread_t thread, int node) {
    struct bitmask* cpumask = numa_allocate_cpumask();
    if (numa_node_to_cpus(node, cpumask) == 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (unsigned int i = 0; i < cpumask->size; i++) {
            if (numa_bitmask_isbitset(cpumask, i)) {
                CPU_SET(i, &cpuset);
            }
        }
        pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    }
    numa_bitmask_free(cpumask);
}

}  // namespace numa
}  // namespace chem
