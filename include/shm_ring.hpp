// shm_ring.hpp — lock-free SPSC ring that lives entirely inside a shared-memory
// region, usable across processes. Project Phase 4 (Team 16).
//
// Two types:
//   ShmSpsc<T, N>  the ring the gateway and strategy processes use: fixed-size,
//                  trivially copyable messages, zero-copy claim/commit and
//                  peek/release, plus copying push/pop.
//   ShmRing        the course CI contract: CAPACITY, init(), push(uint64_t),
//                  pop(uint64_t&) — an ShmSpsc<uint64_t, 1024>.
//
// Rules for living in shared memory
//   * No pointers anywhere: the region is mapped at a different address in each
//     process. Slots are addressed by index (counter & (N - 1)).
//   * The atomics must be lock-free, so they are "address-free" and work across
//     processes (static_assert below).
//   * The region is zero-filled by mmap/ftruncate; init() is called once by the
//     creator before any other process attaches (or before fork()).
//
// Algorithm (same as include/spsc_ring.hpp)
//   * Monotonic 64-bit head_/tail_ counters; size = tail - head; all N slots used.
//   * Producer: write slot, then tail_.store(release). Consumer: tail_.load(acquire),
//     then read slot, then head_.store(release). Each side reads its own counter
//     relaxed.
//   * Each side caches the other side's counter and refreshes it only when the
//     ring looks full / empty. A cache is always <= the real value, so a cache left
//     behind by a crashed process is stale-but-safe for its restarted successor.
//   * Producer line (tail_, head_cache_), consumer line (head_, tail_cache_) and the
//     slots start on separate 64-byte lines: no false sharing between the processes.
//   * A crash between writing a slot and commit() publishes nothing; a crash between
//     reading a slot and release() leaves the message for the next consumer.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

template <class T, std::uint32_t N>
struct ShmSpsc {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
  static_assert(std::is_trivially_copyable<T>::value,
                "shared-memory messages must be trivially copyable (no pointers)");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "cross-process atomics must be lock-free");

  static constexpr std::uint32_t CAPACITY = N;

  // Creator only, once, before any other process uses the ring.
  void init() {
    head_cache_ = 0;
    tail_cache_ = 0;
    head_.store(0, std::memory_order_release);
    tail_.store(0, std::memory_order_release);
  }

  // ── producer ─────────────────────────────────────────────────────────────
  // Slot to fill in place, or nullptr if full. Publish it with commit().
  T* claim() {
    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_cache_ == N) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ == N) return nullptr;
    }
    return &buf_[t & (N - 1)];
  }
  void commit() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  bool push(const T& v) {
    T* slot = claim();
    if (!slot) return false;
    *slot = v;
    commit();
    return true;
  }

  // ── consumer ─────────────────────────────────────────────────────────────
  // Oldest message, read in place, or nullptr if empty. Free it with release().
  const T* peek() {
    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return nullptr;
    }
    return &buf_[h & (N - 1)];
  }
  void release() { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  bool pop(T& out) {
    const T* slot = peek();
    if (!slot) return false;
    out = *slot;
    release();
    return true;
  }

  // Snapshots (exact for the calling side's own decision).
  std::uint64_t size() const {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }
  bool empty() const { return size() == 0; }
  bool full() const { return size() == N; }

 private:
  alignas(64) std::atomic<std::uint64_t> tail_;  // producer's line
  std::uint64_t head_cache_;
  alignas(64) std::atomic<std::uint64_t> head_;  // consumer's line
  std::uint64_t tail_cache_;
  alignas(64) T buf_[N];
};

// Course CI contract: a uint64_t ring of fixed capacity, POD, no pointers.
struct ShmRing {
  static constexpr std::uint32_t CAPACITY = 1024;  // power of two
  void init() { ring_.init(); }                    // creator, once, before fork
  bool push(std::uint64_t v) { return ring_.push(v); }   // producer; false if full
  bool pop(std::uint64_t& out) { return ring_.pop(out); }  // consumer; false if empty
  ShmSpsc<std::uint64_t, CAPACITY> ring_;
};
