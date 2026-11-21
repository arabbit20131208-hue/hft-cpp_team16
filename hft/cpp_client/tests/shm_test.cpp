// shm_test.cpp — cross-process checks for the shared-memory layer.
//   1. ShmRing (CI contract) basics: empty, full, FIFO, wrap.
//   2. ShmSpsc<InMsg> across fork(): 200k full-size messages, zero-copy
//      claim/commit -> peek/release, every byte checked.
//   3. Consumer crash and restart: a consumer process is SIGKILLed after
//      reading a message but before releasing it; a new consumer continues from
//      the same ring and gets that message first, nothing lost or repeated.
//   4. Mapping: create + attach map the same region at different addresses.
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shm_layout.hpp"
#include "spin.hpp"

namespace {

template <class T>
T* shared_anon() {
  void* p = mmap(nullptr, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(p != MAP_FAILED);
  return static_cast<T*>(p);
}

void test_shm_ring_basics() {
  auto* r = shared_anon<ShmRing>();
  r->init();
  std::uint64_t v = 0;
  assert(!r->pop(v));
  for (std::uint64_t i = 0; i < ShmRing::CAPACITY; ++i) assert(r->push(i));
  assert(!r->push(1));
  for (std::uint64_t i = 0; i < ShmRing::CAPACITY; ++i) assert(r->pop(v) && v == i);
  assert(!r->pop(v));
  for (std::uint64_t i = 0; i < 100000; ++i) {  // wrap many times
    assert(r->push(i) && r->pop(v) && v == i);
  }
  munmap(r, sizeof(ShmRing));
}

using InRing = ShmSpsc<ipc::InMsg, 1024>;

void fill(ipc::InMsg& m, std::uint64_t i) {
  m.seq = i;
  m.bid_count = static_cast<std::uint8_t>(i % ipc::kShmDepth);
  for (int k = 0; k < ipc::kShmDepth; ++k) m.bids[k] = {static_cast<double>(i + k), static_cast<int>(i ^ k)};
  std::snprintf(m.symbol, sizeof(m.symbol), "S%llu", static_cast<unsigned long long>(i % 97));
}
bool check(const ipc::InMsg& m, std::uint64_t i) {
  if (m.seq != i || m.bid_count != i % ipc::kShmDepth) return false;
  for (int k = 0; k < ipc::kShmDepth; ++k)
    if (m.bids[k].price != static_cast<double>(i + k) || m.bids[k].quantity != static_cast<int>(i ^ k))
      return false;
  char sym[16];
  std::snprintf(sym, sizeof(sym), "S%llu", static_cast<unsigned long long>(i % 97));
  return std::strcmp(sym, m.symbol) == 0;
}

// Forked children arm their own alarm: if the parent fails an assert, no child
// is left spinning on a full ring (and holding the test runner's pipe).
void produce(InRing* r, std::uint64_t n) {
  alarm(60);
  hft::Backoff b;
  for (std::uint64_t i = 1; i <= n;) {
    ipc::InMsg* s = r->claim();
    if (!s) {
      b.idle();
      continue;
    }
    fill(*s, i++);
    r->commit();
  }
}

void test_typed_ring_across_fork() {
  auto* r = shared_anon<InRing>();
  r->init();
  const std::uint64_t n = 200000;
  const pid_t pid = fork();
  if (pid == 0) {
    produce(r, n);
    _exit(0);
  }
  hft::Backoff b;
  for (std::uint64_t i = 1; i <= n;) {
    const ipc::InMsg* m = r->peek();
    if (!m) {
      b.idle();
      continue;
    }
    assert(check(*m, i));
    r->release();
    ++i;
  }
  int st = 0;
  waitpid(pid, &st, 0);
  assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  munmap(r, sizeof(InRing));
}

// The consumer side survives its owner's death. A consumer process reads
// message `crash_at` and is SIGKILLed before release(), the worst moment. A
// fresh consumer must then get that same message first: nothing lost, nothing
// repeated, even though head_ and the dead process's tail cache stay in the ring.
void test_consumer_restart() {
  struct Shared {
    InRing ring;
    std::atomic<std::uint64_t> consumed;
  };
  auto* s = shared_anon<Shared>();
  s->ring.init();
  s->consumed.store(0);
  const std::uint64_t n = 100000, crash_at = n / 3;

  const pid_t producer = fork();
  if (producer == 0) {
    produce(&s->ring, n);
    _exit(0);
  }
  const pid_t first = fork();
  if (first == 0) {
    alarm(60);
    hft::Backoff b;
    for (std::uint64_t i = 1; i <= n;) {
      const ipc::InMsg* m = s->ring.peek();
      if (!m) {
        b.idle();
        continue;
      }
      if (!check(*m, i)) _exit(1);
      if (i == crash_at) raise(SIGKILL);  // read but not released
      s->ring.release();
      s->consumed.store(i++);
    }
    _exit(0);
  }
  int st = 0;
  waitpid(first, &st, 0);
  assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
  assert(s->consumed.load() == crash_at - 1);

  hft::Backoff b;  // the restarted consumer, in this process
  for (std::uint64_t i = crash_at; i <= n;) {
    const ipc::InMsg* m = s->ring.peek();
    if (!m) {
      b.idle();
      continue;
    }
    assert(check(*m, i));
    s->ring.release();
    ++i;
  }
  waitpid(producer, &st, 0);
  assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  std::printf("  consumer killed holding message %llu of %llu; restart got it again, no loss\n",
              static_cast<unsigned long long>(crash_at), static_cast<unsigned long long>(n));
  munmap(s, sizeof(Shared));
}

void test_mapping() {
  const std::string name = "/hft16_test_" + std::to_string(getpid());
  ipc::Mapping creator, attacher;
  assert(creator.create(name, true));
  assert(attacher.attach(name));
  assert(creator.region() != nullptr && attacher.region() != nullptr);
  ipc::OutMsg o{};
  o.seq = 42;
  ipc::put(o.symbol, "AAPL");
  assert(creator.region()->out.push(o));
  ipc::OutMsg got{};
  assert(attacher.region()->out.pop(got) && got.seq == 42 && ipc::get(got.symbol) == "AAPL");
  assert(attacher.region()->ctl.replay == 1);
  ipc::Mapping late;
  attacher.close();
  creator.close();  // unlinks
  assert(!late.attach(name));
}

}  // namespace

int main() {
  static_assert(sizeof(ipc::InMsg) % 64 == 0 && alignof(ipc::InMsg) == 64, "InMsg cache-line sized");
  static_assert(offsetof(ipc::Control, gw_heartbeat_ns) % 64 == 0, "gateway line aligned");
  static_assert(offsetof(ipc::Control, st_heartbeat_ns) % 64 == 0, "strategy line aligned");
  test_shm_ring_basics();
  test_typed_ring_across_fork();
  test_consumer_restart();
  test_mapping();
  std::printf("shm_test: ShmRing basics, 200k InMsg (%zu B) across fork, consumer kill/restart, "
              "shm_open mapping passed.\n",
              sizeof(ipc::InMsg));
}
