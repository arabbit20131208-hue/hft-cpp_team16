// spin.hpp — small helpers shared by the gateway and strategy processes:
// a monotonic ns clock, spin-then-yield backoff, core pinning, and a fixed
// latency histogram (no allocation after construction).
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace hft {

// steady_clock is system-wide monotonic (CLOCK_MONOTONIC on Linux,
// mach_absolute_time on macOS), so timestamps compare across processes.
inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

// Spin for IDLE_SPINS empty polls (default 4096), then yield once. Only used
// when there is no work, so it never delays a message that is ready.
inline unsigned idle_spin_limit() {
  static const unsigned limit = [] {
    const char* v = std::getenv("IDLE_SPINS");
    return v && *v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 4096u;
  }();
  return limit;
}

struct Backoff {
  unsigned spins = 0;
  void idle() {
    if (++spins < idle_spin_limit()) {
      cpu_relax();
    } else {
      spins = 0;
      std::this_thread::yield();
    }
  }
  void reset() { spins = 0; }
};

// Linux only; macOS has no hard-affinity API (returns false, thread floats).
inline bool pin_current_thread(int core) {
#if defined(__linux__)
  if (core < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)core;
  return false;
#endif
}

inline int env_int(const char* key, int def) {
  const char* v = std::getenv(key);
  return v && *v ? std::atoi(v) : def;
}

// Latency histogram: 32 ns buckets up to ~131 µs, then a coarse tail of
// 64 µs buckets up to ~2 s. Percentiles are bucket upper bounds.
class LatencyHist {
 public:
  void add(std::int64_t ns) {
    if (ns < 0) ns = 0;
    ++count_;
    if (ns < kFine * kFineWidth) {
      ++fine_[static_cast<std::size_t>(ns / kFineWidth)];
    } else {
      const std::int64_t i = (ns - kFine * kFineWidth) / kCoarseWidth;
      ++coarse_[static_cast<std::size_t>(i < kCoarse ? i : kCoarse - 1)];
    }
  }
  std::int64_t count() const { return count_; }
  // q in [0, 100].
  std::int64_t percentile_ns(double q) const {
    if (count_ == 0) return 0;
    const auto rank = static_cast<std::int64_t>(q / 100.0 * static_cast<double>(count_ - 1));
    std::int64_t seen = 0;
    for (std::size_t i = 0; i < fine_.size(); ++i)
      if ((seen += fine_[i]) > rank) return static_cast<std::int64_t>(i + 1) * kFineWidth;
    for (std::size_t i = 0; i < coarse_.size(); ++i)
      if ((seen += coarse_[i]) > rank)
        return kFine * kFineWidth + static_cast<std::int64_t>(i + 1) * kCoarseWidth;
    return kFine * kFineWidth + kCoarse * kCoarseWidth;
  }
  void print(const char* label) const {
    std::fprintf(stderr, "%s n=%lld p50=%lld p90=%lld p99=%lld p99.9=%lld ns\n", label,
                 static_cast<long long>(count_), static_cast<long long>(percentile_ns(50)),
                 static_cast<long long>(percentile_ns(90)), static_cast<long long>(percentile_ns(99)),
                 static_cast<long long>(percentile_ns(99.9)));
  }

 private:
  static constexpr std::int64_t kFine = 4096, kFineWidth = 32;
  static constexpr std::int64_t kCoarse = 32768, kCoarseWidth = 65536;
  std::array<std::int64_t, kFine> fine_{};
  std::array<std::int64_t, kCoarse> coarse_{};
  std::int64_t count_ = 0;
};

}  // namespace hft
