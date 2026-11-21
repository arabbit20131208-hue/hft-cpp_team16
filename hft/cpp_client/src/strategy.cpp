// strategy.cpp — the Strategy process.
//
// Attaches to the gateway's shared-memory region (HFT_SHM), claims the
// consumer side of the inbound ring, and runs QueueStrategy on every message:
// book updates -> decisions -> order intents on the outbound ring. It holds no
// socket; if it crashes, the gateway keeps the venue connection, cancels our
// resting orders, and starts a new strategy, which picks up where the ring is.
//
// Liveness: writes a heartbeat every loop; exits when the gateway asks
// (ctl.shutdown) or when the gateway's heartbeat goes stale (orphaned).
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "queue_strategy.hpp"
#include "quota.hpp"
#include "shm_layout.hpp"
#include "spin.hpp"

namespace {

using ipc::InMsg;
using ipc::InType;
using ipc::OutMsg;
using ipc::OutType;

constexpr std::int64_t kGatewayTimeoutNs = 3'000'000'000;  // orphaned after 3 s
constexpr std::int64_t kStaleBookNs = 200'000'000;         // live: skip books older than 200 ms

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// The strategy's view of the outside world: intents go to the outbound ring,
// stamped with the inbound message they answer.
class ShmSink : public arena::OrderSink {
 public:
  ShmSink(ipc::Region& r, int quota, long long window_ms)
      : r_(r), quota_(quota, window_ms * 1'000'000LL) {}

  void set_origin(const InMsg& m, std::int64_t t_pop) {
    seq_ = m.seq;
    t_recv_ = m.t_recv_ns;
    t_in_push_ = m.t_push_ns;
    t_in_pop_ = t_pop;
  }

  void place_limit(std::string_view symbol, bool buy, int quantity, double price) override {
    OutMsg& o = claim();
    o.type = OutType::kPlace;
    o.side = buy ? 'b' : 's';
    o.market = false;
    o.quantity = quantity;
    o.price = price;
    ipc::put(o.symbol, symbol);
    o.order_id[0] = '\0';
    commit(o);
  }
  void cancel(std::string_view order_id, std::string_view symbol) override {
    OutMsg& o = claim();
    o.type = OutType::kCancel;
    o.side = 0;
    o.market = false;
    o.quantity = 0;
    o.price = 0;
    ipc::put(o.symbol, symbol);
    ipc::put(o.order_id, order_id);
    commit(o);
  }
  void done() {
    OutMsg& o = claim();
    o.type = OutType::kDone;
    o.symbol[0] = o.order_id[0] = '\0';
    commit(o);
  }
  bool quota_allows(bool cancel) override { return quota_.allow(t_recv_, cancel); }
  void request_stop() override { stop_requested = true; }

  const arena::QuotaGuard& quota() const { return quota_; }
  bool stop_requested = false;

 private:
  // Back-pressure: wait for the gateway to drain the outbound ring.
  OutMsg& claim() {
    hft::Backoff b;
    OutMsg* o;
    while (!(o = r_.out.claim())) b.idle();
    return *o;
  }
  void commit(OutMsg& o) {
    o.seq = seq_;
    o.t_recv_ns = t_recv_;
    o.t_in_push_ns = t_in_push_;
    o.t_in_pop_ns = t_in_pop_;
    o.t_out_push_ns = hft::now_ns();
    r_.out.commit();
  }

  ipc::Region& r_;
  arena::QuotaGuard quota_;
  std::uint64_t seq_ = 0;
  std::int64_t t_recv_ = 0, t_in_push_ = 0, t_in_pop_ = 0;
};

std::string env_or(const char* key, const char* def) {
  const char* v = std::getenv(key);
  return v && *v ? v : def;
}

}  // namespace

int main() {
  const std::string name = env_or("HFT_SHM", "/hft_team16");
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // ── start handshake: attach, then claim the consumer side ─────────────────
  ipc::Mapping map;
  for (int i = 0; !map.attach(name); ++i) {
    if (i == 500 || g_stop) {
      std::fprintf(stderr, "[strategy] no gateway region %s\n", name.c_str());
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ipc::Region& r = *map.region();
  ipc::Control& c = r.ctl;
  const std::int32_t me = static_cast<std::int32_t>(::getpid());
  std::int32_t owner = c.st_pid.load();
  if (owner != 0 && owner != me && ipc::pid_alive(owner)) {
    std::fprintf(stderr, "[strategy] another strategy (pid %d) owns %s\n", owner, name.c_str());
    return 1;
  }
  if (!c.st_pid.compare_exchange_strong(owner, me)) {
    std::fprintf(stderr, "[strategy] lost the race to own %s\n", name.c_str());
    return 1;
  }
  hft::pin_current_thread(hft::env_int("STRATEGY_CORE", -1));

  const char* aware = std::getenv("QUEUE_AWARE");
  arena::PolicyParams params;
  params.queue_aware = !(aware && std::string(aware) == "0");
  ShmSink sink(r, hft::env_int("ORDER_QUOTA", 6), hft::env_int("ORDER_QUOTA_MS", 1000));
  auto bot = std::make_unique<arena::QueueStrategy>(std::stod(env_or("TICK_SIZE", "0.01")), sink,
                                                    params);
  const bool replay = c.replay != 0;
  hft::LatencyHist ipc_in;
  long long stale_books = 0, gaps = 0, messages = 0;
  std::uint64_t last_seq = 0;

  c.st_heartbeat_ns.store(hft::now_ns(), std::memory_order_relaxed);
  const std::uint32_t epoch = c.st_epoch.fetch_add(1) + 1;
  c.st_ready.store(1, std::memory_order_release);
  std::fprintf(stderr, "[strategy] pid %d attached to %s (epoch %u)\n", me, name.c_str(), epoch);

  // ── main loop ───────────────────────────────────────────────────────────
  hft::Backoff backoff;
  unsigned beat = 0;
  while (!g_stop && !sink.stop_requested) {
    if ((++beat & 255) == 0) {  // every 256 loop turns, busy or idle
      const std::int64_t now = hft::now_ns();
      c.st_heartbeat_ns.store(now, std::memory_order_relaxed);
      if (c.shutdown.load(std::memory_order_acquire)) break;
      if (now - c.gw_heartbeat_ns.load(std::memory_order_relaxed) > kGatewayTimeoutNs &&
          !ipc::pid_alive(c.gw_pid.load())) {
        std::fprintf(stderr, "[strategy] gateway gone, exiting\n");
        break;
      }
    }
    const InMsg* m = r.in.peek();
    if (!m) {
      backoff.idle();
      continue;
    }
    backoff.reset();
    const std::int64_t t_pop = hft::now_ns();
    ipc_in.add(t_pop - m->t_push_ns);
    ++messages;
    if (m->seq != 0) {  // resync messages carry seq 0
      if (last_seq != 0 && m->seq != last_seq + 1) gaps += static_cast<long long>(m->seq - last_seq - 1);
      last_seq = m->seq;
    }
    sink.set_origin(*m, t_pop);

    switch (m->type) {
      case InType::kBook:
        if (!replay && t_pop - m->t_recv_ns > kStaleBookNs) {
          ++stale_books;  // e.g. queued while this process was restarting
          break;
        }
        bot->on_book({ipc::get(m->symbol), m->bids, m->bid_count, m->asks, m->ask_count, m->mid,
                      m->spread, m->microprice, m->obi},
                     c.session_open.load(std::memory_order_relaxed) != 0);
        break;
      case InType::kAck:
        bot->on_order_ack(ipc::get(m->order_id), ipc::get(m->symbol),
                          m->side == 'b' ? "buy" : "sell", m->queue_ahead, m->level_qty);
        break;
      case InType::kQueue:
        bot->on_queue_update(ipc::get(m->order_id), ipc::get(m->symbol),
                             m->side == 'b' ? "buy" : "sell", m->queue_ahead, m->level_qty);
        break;
      case InType::kFill:
        bot->on_fill(m->side == 'b' ? "buy" : m->side == 's' ? "sell" : "", ipc::get(m->symbol),
                     m->quantity);
        break;
      case InType::kError:
        bot->on_error(ipc::get(m->code), ipc::get(m->text));
        break;
      case InType::kResync:
        bot->set_position(ipc::get(m->symbol), m->position);
        break;
      case InType::kIgnored:
        break;
    }
    const std::uint64_t seq = m->seq;
    r.in.release();
    if (replay && seq != 0) sink.done();
  }

  // ── stop handshake ────────────────────────────────────────────────────────
  c.st_ready.store(0, std::memory_order_release);
  c.st_pid.store(0);
  bot->print_stats();
  std::fprintf(stderr, "[strategy] messages=%lld gaps=%lld stale_books_skipped=%lld\n", messages,
               gaps, stale_books);
  std::fprintf(stderr, "[quota] order_quota=%d window_ms=%lld max_used_in_window=%d skipped=%lld\n",
               sink.quota().quota(), sink.quota().window_ns() / 1000000,
               sink.quota().max_used_in_window(), sink.quota().skipped());
  ipc_in.print("[ipc] in-ring one-way");
  // 3 = the strategy itself asked to stop (the gateway shuts down instead of restarting).
  return sink.stop_requested ? 3 : 0;
}
