// pipeline.hpp — slot types and thread helpers for the Phase 3 pipeline.
//
//   rx thread ──ticks_──▶ strategy thread ──sends_──▶ send thread
//      ▲                     │      ▲                     │
//      └──────free_in_───────┘      └─────free_out_───────┘
//
// SPSCRing carries uint64 slot indices. The payloads live in two preallocated
// pools (Event, OutSlot). Pushing an index hands the whole slot to the other
// thread; the ring's release/acquire pair orders the slot's contents. Each
// index ring has one producer and one consumer, and the pools never allocate
// after start-up.
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "fast_codec.hpp"

namespace arena {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr std::size_t kInSlots = 256;   // decoded events in flight (power of two)
constexpr std::size_t kOutSlots = 256;  // decided ticks in flight (power of two)
constexpr std::size_t kMaxTickMessages = 16;

inline long long ns_between(TimePoint a, TimePoint b) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

// One spin-wait iteration: tells the core we are spinning (frees pipeline
// resources for a sibling hyperthread) without entering the kernel.
inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

// Spin politely: pure spinning for a short while, then yield to the OS so an
// idle pipeline does not starve other threads on a machine with few cores.
// Only reached when there is no work, never between receiving and sending.
// IDLE_SPINS sets how long to spin before yielding (default 4096; use a
// small value on machines with fewer free cores than pipeline threads).
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

// Pin the calling thread to one core. Linux only; macOS exposes no hard
// affinity API, so this returns false there and the thread floats.
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

enum class EventType : std::uint8_t {
  kIgnored,  // unknown or blank frame (still forwarded in replay: one output line per input)
  kBook,
  kAck,
  kQueue,
  kFill,
  kPortfolio,
  kSession,
  kError,
  kStop,  // replay EOF: drains the pipeline
};

// One inbound message, decoded on the rx thread. Strings are reserved once,
// so assignments on the rx thread and reads on the strategy thread never
// allocate.
struct Event {
  EventType type = EventType::kIgnored;
  TimePoint recv{}, pushed{};  // frame arrival, handed to the ring
  BookView book;               // kBook
  std::string order_id, symbol, side, code, text;
  double price = 0, cash = 0, net_worth = 0, realized = 0, unrealized = 0;
  int quantity = 0, queue_ahead = -1, level_qty = 0;
  bool maker = false;

  Event() {
    book.symbol.reserve(128);
    for (auto* s : {&order_id, &symbol, &side, &code, &text}) s->reserve(256);
  }
};

// The wire messages one event produced, plus its timestamps.
struct OutSlot {
  std::array<std::string, kMaxTickMessages> msgs;
  std::size_t count = 0;
  bool book = false;  // stamped for LAT/PROF
  bool stop = false;  // last slot of a replay
  std::string symbol;
  TimePoint recv{}, pushed{}, popped{}, decided{};

  OutSlot() {
    for (auto& m : msgs) m.reserve(512);
    symbol.reserve(128);
  }
};

}  // namespace arena
