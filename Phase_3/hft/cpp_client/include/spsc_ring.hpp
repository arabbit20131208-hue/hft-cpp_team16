// spsc_ring.hpp — single-producer / single-consumer lock-free ring buffer.
// HW10 / Project Phase 3 (Team 16).
//
// Contract (course CI): SPSCRing(capacity_pow2), push(v) -> false if full,
// pop(out) -> false if empty, empty(), full().
//
// Design
//   * head_/tail_ are monotonically increasing counters, masked on access, so
//     size is tail - head and all `capacity` slots are usable.
//   * Exactly one thread calls push() (owns tail_) and one calls pop() (owns
//     head_), so plain atomic loads/stores suffice — no CAS.
//   * push: write the slot, then tail_.store(release). pop: tail_.load(acquire),
//     then read the slot. That release/acquire pair is what makes the payload
//     visible to the consumer; the mirror pair on head_ hands freed slots back.
//   * Each side keeps a private cache of the other side's counter and re-reads
//     the shared atomic only when the ring looks full (producer) or empty
//     (consumer). In steady state each call touches only its own cache line.
//   * Producer data (tail_, head_cache_) and consumer data (head_, tail_cache_)
//     sit on separate 64-byte lines, so the two cores never false-share.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

struct SPSCRing {
  // `capacity_pow2` should be a power of two; other values are rounded up.
  explicit SPSCRing(std::size_t capacity_pow2)
      : cap_(round_up(capacity_pow2)), mask_(cap_ - 1), buf_(new std::uint64_t[cap_]()) {}

  SPSCRing(const SPSCRing&) = delete;
  SPSCRing& operator=(const SPSCRing&) = delete;

  // Producer thread only. False if full.
  bool push(std::uint64_t v) {
    const std::size_t t = tail_.load(std::memory_order_relaxed);  // we own tail_
    if (t - head_cache_ == cap_) {                                 // looks full: refresh
      head_cache_ = head_.load(std::memory_order_acquire);         // see slots the consumer freed
      if (t - head_cache_ == cap_) return false;
    }
    buf_[t & mask_] = v;                            // write the payload...
    tail_.store(t + 1, std::memory_order_release);  // ...then publish it
    return true;
  }

  // Consumer thread only. False if empty.
  bool pop(std::uint64_t& out) {
    const std::size_t h = head_.load(std::memory_order_relaxed);  // we own head_
    if (h == tail_cache_) {                                        // looks empty: refresh
      tail_cache_ = tail_.load(std::memory_order_acquire);         // see the producer's writes
      if (h == tail_cache_) return false;
    }
    out = buf_[h & mask_];                          // read the payload...
    head_.store(h + 1, std::memory_order_release);  // ...then hand the slot back
    return true;
  }

  // Snapshots; exact when called from either owning thread for its own
  // decision (the other side can only make the ring less full / less empty).
  bool empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }
  bool full() const {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire) ==
           cap_;
  }
  std::size_t capacity() const { return cap_; }

 private:
  static std::size_t round_up(std::size_t n) {
    std::size_t c = 2;
    while (c < n) c <<= 1;
    return c;
  }

  // Read-only after construction (shared freely).
  const std::size_t cap_, mask_;
  const std::unique_ptr<std::uint64_t[]> buf_;

  // Producer's cache line.
  alignas(64) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_ = 0;

  // Consumer's cache line.
  alignas(64) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;
};
