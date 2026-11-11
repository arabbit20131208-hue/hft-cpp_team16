// pipeline_test.cpp — SPSC ring, slot-pool handoff, and order-quota checks.
//
// The handoff test is the pattern the bot uses: a pool of plain (non-atomic)
// structs, an index ring forward and a free-list ring back. Built with
// -DHFT_TSAN=ON it proves the ring's release/acquire pairs order the slot
// contents (TSan reports any unsynchronised slot access).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "pipeline.hpp"
#include "quota.hpp"
#include "spsc_ring.hpp"

namespace {

void test_ring_basics() {
  SPSCRing r(8);
  std::uint64_t x = 0;
  assert(r.empty() && !r.full() && !r.pop(x));
  for (std::uint64_t i = 0; i < 8; ++i) assert(r.push(i));
  assert(r.full() && !r.push(99));
  for (std::uint64_t i = 0; i < 8; ++i) assert(r.pop(x) && x == i);
  assert(r.empty());
  SPSCRing odd(100);  // rounded up to a power of two
  assert(odd.capacity() == 128);
}

struct Payload {
  std::uint64_t seq = 0;
  std::uint64_t words[30]{};  // ~250 bytes of plain data written by the producer
};

void test_slot_handoff(std::uint64_t n) {
  constexpr std::size_t kSlots = 64;
  std::unique_ptr<Payload[]> pool(new Payload[kSlots]);
  SPSCRing work(kSlots), free_list(kSlots);
  for (std::uint64_t i = 0; i < kSlots; ++i) free_list.push(i);
  bool ok = true;

  std::thread consumer([&] {
    arena::Backoff b;
    for (std::uint64_t expect = 0; expect < n;) {
      std::uint64_t slot;
      if (!work.pop(slot)) {
        b.idle();
        continue;
      }
      const Payload& p = pool[slot];
      if (p.seq != expect) ok = false;
      for (std::uint64_t w : p.words)
        if (w != expect * 31) ok = false;
      while (!free_list.push(slot)) arena::cpu_relax();
      ++expect;
    }
  });
  arena::Backoff b;
  for (std::uint64_t i = 0; i < n;) {
    std::uint64_t slot;
    if (!free_list.pop(slot)) {
      b.idle();
      continue;
    }
    Payload& p = pool[slot];
    p.seq = i;
    for (auto& w : p.words) w = i * 31;
    while (!work.push(slot)) arena::cpu_relax();
    ++i;
  }
  consumer.join();
  assert(ok);
}

void test_quota() {
  const long long ms = 1000000;
  arena::QuotaGuard q(3, 1000 * ms, 1);  // 3 per second, 1 kept for cancels
  assert(q.allow(0, false) && q.allow(1, false));
  assert(!q.allow(2, false));  // third message is the cancel reserve
  assert(q.allow(3, true));    // ...which a cancel may use
  assert(!q.allow(4, true));   // window spent
  assert(q.allow(1000 * ms, false));  // next window
  assert(q.max_used_in_window() == 3 && q.skipped() == 2);

  arena::QuotaGuard off(0, 1000 * ms);
  for (int i = 0; i < 100; ++i) assert(off.allow(i, false));
}

}  // namespace

int main() {
  test_ring_basics();
  test_slot_handoff(200000);
  test_quota();
  std::puts("pipeline_test: ring, cross-thread slot handoff (200k) and order quota passed.");
}
