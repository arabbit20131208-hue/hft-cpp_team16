// shm_layout.hpp — the shared-memory region between the gateway and strategy
// processes: fixed-size POD messages, a control block, and the two rings.
//
//   gateway ──in  (InMsg:  book updates, acks, fills, resync)──▶ strategy
//   gateway ◀─out (OutMsg: order intents, replay "done" marks)── strategy
//
// Everything here is trivially copyable with fixed-size char arrays instead
// of strings: the region is mapped at a different address in each process,
// so it must not contain pointers.
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "local_book.hpp"  // arena::Level
#include "shm_ring.hpp"

namespace ipc {

constexpr std::uint64_t kMagic = 0x3631544648ULL;  // "HFT16"
constexpr std::uint32_t kVersion = 1;
constexpr int kShmDepth = 32;  // levels per side carried in a book update
constexpr std::uint32_t kInCapacity = 1024;
constexpr std::uint32_t kOutCapacity = 1024;

template <std::size_t N>
inline void put(char (&dst)[N], std::string_view src) {
  const std::size_t n = src.size() < N - 1 ? src.size() : N - 1;
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}
template <std::size_t N>
inline std::string_view get(const char (&src)[N]) {
  return std::string_view(src, strnlen(src, N));
}

enum class InType : std::uint8_t {
  kIgnored,  // replay line with nothing for the strategy (still answered)
  kBook,
  kAck,
  kQueue,
  kFill,
  kError,
  kResync,  // gateway's position for one symbol, sent to a (re)started strategy
};

// Gateway -> strategy. One normalized, fixed-layout message per inbound frame.
struct alignas(64) InMsg {
  std::uint64_t seq;       // gateway frame number (replay: input line number, from 1)
  std::int64_t t_recv_ns;  // frame arrival at the gateway
  std::int64_t t_push_ns;  // committed to the ring
  InType type;
  std::uint8_t bid_count, ask_count;
  char side;  // 'b' / 's' / 0
  std::int32_t quantity, queue_ahead, level_qty, position;
  double best_bid, best_ask, mid, spread, microprice, obi, price;
  char symbol[16];
  char order_id[48];
  char code[32];
  char text[96];
  arena::Level bids[kShmDepth], asks[kShmDepth];
};

enum class OutType : std::uint8_t {
  kPlace,
  kCancel,
  kDone,  // replay: the strategy finished input line `seq`
};

// Strategy -> gateway. Carries the origin timestamps so the gateway can stamp
// tick-to-trade and split it into stages.
struct alignas(64) OutMsg {
  std::uint64_t seq;
  std::int64_t t_recv_ns, t_in_push_ns, t_in_pop_ns, t_out_push_ns;
  OutType type;
  char side;  // 'b' / 's'
  bool market;
  std::int32_t quantity;
  double price;
  char symbol[16];
  char order_id[48];
};

static_assert(std::is_trivially_copyable<InMsg>::value, "InMsg must be POD-like");
static_assert(std::is_trivially_copyable<OutMsg>::value, "OutMsg must be POD-like");

// Liveness and handshake. Each process writes only its own cache line.
struct Control {
  std::atomic<std::uint64_t> magic;  // written last by the creator (release)
  std::uint32_t version;
  std::uint32_t replay;  // 1: strategy answers every input with kDone
  alignas(64) std::atomic<std::int64_t> gw_heartbeat_ns;
  std::atomic<std::int32_t> gw_pid;
  std::atomic<std::uint32_t> shutdown;      // gateway asks the strategy to exit
  std::atomic<std::uint32_t> session_open;  // venue session state, mirrored
  alignas(64) std::atomic<std::int64_t> st_heartbeat_ns;
  std::atomic<std::int32_t> st_pid;      // owner of the consumer side of `in`
  std::atomic<std::uint32_t> st_epoch;   // +1 on every strategy start
  std::atomic<std::uint32_t> st_ready;   // strategy attached and consuming
};

struct Region {
  Control ctl;
  ShmSpsc<InMsg, kInCapacity> in;
  ShmSpsc<OutMsg, kOutCapacity> out;
};

inline bool pid_alive(std::int32_t pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

// POSIX shared memory (shm_open + mmap). The gateway creates; the strategy attaches.
class Mapping {
 public:
  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  ~Mapping() { close(); }

  // Gateway: create a fresh region (replacing a stale one) and initialize it.
  bool create(const std::string& name, bool replay) {
    ::shm_unlink(name.c_str());
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return fail("shm_open(create)");
    if (::ftruncate(fd, static_cast<off_t>(sizeof(Region))) != 0) {
      ::close(fd);
      return fail("ftruncate");
    }
    if (!map(fd)) return false;
    name_ = name;
    owner_ = true;
    Control& c = region_->ctl;
    c.version = kVersion;
    c.replay = replay ? 1 : 0;
    c.gw_heartbeat_ns.store(0);
    c.gw_pid.store(static_cast<std::int32_t>(::getpid()));
    c.shutdown.store(0);
    c.session_open.store(replay ? 1 : 0);
    c.st_heartbeat_ns.store(0);
    c.st_pid.store(0);
    c.st_epoch.store(0);
    c.st_ready.store(0);
    region_->in.init();
    region_->out.init();
    c.magic.store(kMagic, std::memory_order_release);  // last: attachers check it
    return true;
  }

  // Strategy: attach to an existing, initialized region.
  bool attach(const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) return false;  // not created yet: caller retries
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(Region))) {
      ::close(fd);
      return false;
    }
    if (!map(fd)) return false;
    if (region_->ctl.magic.load(std::memory_order_acquire) != kMagic ||
        region_->ctl.version != kVersion) {
      close();
      return false;
    }
    name_ = name;
    return true;
  }

  void close() {
    if (region_) ::munmap(region_, sizeof(Region));
    region_ = nullptr;
    if (owner_) ::shm_unlink(name_.c_str());
    owner_ = false;
  }

  Region* region() const { return region_; }
  const std::string& name() const { return name_; }

 private:
  bool map(int fd) {
    void* p = ::mmap(nullptr, sizeof(Region), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return fail("mmap");
    region_ = static_cast<Region*>(p);
    return true;
  }
  static bool fail(const char* what) {
    std::perror(what);
    return false;
  }

  Region* region_ = nullptr;
  std::string name_;
  bool owner_ = false;
};

}  // namespace ipc
