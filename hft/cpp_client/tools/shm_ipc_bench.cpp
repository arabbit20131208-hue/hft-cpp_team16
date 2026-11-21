// shm_ipc_bench.cpp — one-way IPC latency and throughput across the real
// shared-memory rings, between two processes (fork).
//
//   one-way   producer stamps now_ns() into each message right before commit(),
//             consumer subtracts on peek(). Messages are paced (default 1 per
//             10 µs) so the ring is near empty: this is the hop cost, not
//             queueing. Both processes read the same monotonic clock.
//   ping-pong round trip over two rings (producer -> consumer -> producer) / 2,
//             a check that does not depend on cross-process clock reads.
//   flat-out  messages per second with the producer never pausing; each
//             message is written and read in full (struct copy, checksummed).
//
// Run for two payloads: the 8-byte CI ring (ShmRing) and the 1,344-byte book
// update (InMsg) the gateway sends.
//
// Usage: shm_ipc_bench [n=200000] [pace_ns=10000]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "shm_layout.hpp"
#include "spin.hpp"

namespace {

struct Small {
  std::int64_t t;
  std::uint64_t seq;
};

template <class T>
struct Pair {
  ShmSpsc<T, 1024> a, b;  // a: parent -> child, b: child -> parent
  std::atomic<int> ready;
};

template <class T>
Pair<T>* make_pair() {
  void* p = mmap(nullptr, sizeof(Pair<T>), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    std::perror("mmap");
    std::exit(1);
  }
  auto* q = static_cast<Pair<T>*>(p);
  q->a.init();
  q->b.init();
  q->ready.store(0);
  return q;
}

std::int64_t& stamp(Small& m) { return m.t; }
std::int64_t& stamp(ipc::InMsg& m) { return m.t_push_ns; }

void report(const char* what, const char* payload, std::vector<std::int64_t>& v) {
  std::sort(v.begin(), v.end());
  auto p = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q / 100 * v.size()))]; };
  std::printf("%-10s %-14s n=%zu p50=%lld p90=%lld p99=%lld p99.9=%lld max=%lld ns\n", what, payload,
              v.size(), static_cast<long long>(p(50)), static_cast<long long>(p(90)),
              static_cast<long long>(p(99)), static_cast<long long>(p(99.9)),
              static_cast<long long>(v.back()));
}

template <class T>
void one_way(const char* payload, std::size_t n, std::int64_t pace_ns) {
  auto* q = make_pair<T>();
  const pid_t pid = fork();
  if (pid == 0) {  // consumer: measure, then send results back over ring b as Small-sized stamps
    std::vector<std::int64_t> lat;
    lat.reserve(n);
    hft::Backoff b;
    q->ready.store(1);
    while (lat.size() < n) {
      const T* m = q->a.peek();
      if (!m) {
        b.idle();
        continue;
      }
      const std::int64_t t = hft::now_ns();
      lat.push_back(t - stamp(const_cast<T&>(*m)));
      q->a.release();
    }
    const std::size_t warm = n / 20;
    std::vector<std::int64_t> v(lat.begin() + static_cast<std::ptrdiff_t>(warm), lat.end());
    report("one-way", payload, v);
    std::fflush(stdout);
    _exit(0);
  }
  while (!q->ready.load()) hft::cpu_relax();
  std::int64_t next = hft::now_ns();
  for (std::size_t i = 0; i < n; ++i) {
    while (hft::now_ns() < next) hft::cpu_relax();
    next += pace_ns;
    T* s;
    while (!(s = q->a.claim())) hft::cpu_relax();
    stamp(*s) = hft::now_ns();
    q->a.commit();
  }
  int st;
  waitpid(pid, &st, 0);
  munmap(q, sizeof(Pair<T>));
}

template <class T>
void ping_pong(const char* payload, std::size_t n) {
  auto* q = make_pair<T>();
  const pid_t pid = fork();
  if (pid == 0) {  // echo
    for (std::size_t i = 0; i < n; ++i) {
      const T* m;
      while (!(m = q->a.peek())) hft::cpu_relax();
      T* out;
      while (!(out = q->b.claim())) hft::cpu_relax();
      *out = *m;
      q->a.release();
      q->b.commit();
    }
    _exit(0);
  }
  std::vector<std::int64_t> half_rtt;
  half_rtt.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    T* s;
    while (!(s = q->a.claim())) hft::cpu_relax();
    const std::int64_t t0 = hft::now_ns();
    stamp(*s) = t0;
    q->a.commit();
    const T* back;
    while (!(back = q->b.peek())) hft::cpu_relax();
    half_rtt.push_back((hft::now_ns() - t0) / 2);
    q->b.release();
  }
  int st;
  waitpid(pid, &st, 0);
  std::vector<std::int64_t> v(half_rtt.begin() + static_cast<std::ptrdiff_t>(n / 20), half_rtt.end());
  report("rtt/2", payload, v);
  munmap(q, sizeof(Pair<T>));
}

// Every message is fully written by the producer and fully read by the
// consumer (struct copy), so the rate includes moving the payload.
template <class T>
void flat_out(const char* payload, std::size_t n) {
  auto* q = make_pair<T>();
  const pid_t pid = fork();
  if (pid == 0) {
    T msg{};
    for (std::size_t i = 0; i < n; ++i) {
      T* s;
      while (!(s = q->a.claim())) hft::cpu_relax();
      stamp(msg) = static_cast<std::int64_t>(i);
      *s = msg;
      q->a.commit();
    }
    _exit(0);
  }
  T got{};
  std::int64_t check = 0;
  const std::int64_t t0 = hft::now_ns();
  for (std::size_t i = 0; i < n; ++i) {
    const T* m;
    while (!(m = q->a.peek())) hft::cpu_relax();
    got = *m;
    check += stamp(got);
    q->a.release();
  }
  const double secs = (hft::now_ns() - t0) / 1e9;
  int st;
  waitpid(pid, &st, 0);
  const std::int64_t expect = static_cast<std::int64_t>(n) * static_cast<std::int64_t>(n - 1) / 2;
  std::printf("%-10s %-14s n=%zu %.0f msgs/s (%.0f MB/s)%s\n", "flat-out", payload, n, n / secs,
              n * sizeof(T) / secs / 1e6, check == expect ? "" : "  CHECKSUM MISMATCH");
  munmap(q, sizeof(Pair<T>));
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 200000;
  const std::int64_t pace = argc > 2 ? std::atoll(argv[2]) : 10000;
  std::printf("shm_ipc_bench: n=%zu pace=%lld ns, InMsg=%zu B\n", n, static_cast<long long>(pace),
              sizeof(ipc::InMsg));
  std::fflush(stdout);
  one_way<Small>("16 B", n, pace);
  one_way<ipc::InMsg>("InMsg 1344 B", n, pace);
  ping_pong<Small>("16 B", n);
  ping_pong<ipc::InMsg>("InMsg 1344 B", n);
  flat_out<Small>("16 B", n * 10);
  flat_out<ipc::InMsg>("InMsg 1344 B", n);
}
