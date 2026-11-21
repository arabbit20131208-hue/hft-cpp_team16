// gateway.cpp — the Gateway process (built as `hft_bot`).
//
// Owns the only WebSocket to the arena (or stdin/stdout in --replay), and the
// shared-memory region. Three threads:
//   rx          socket callback / stdin reader: stamp, decode, write one
//               fixed-layout InMsg per frame into the inbound ring.
//   tx          drain the outbound ring: encode place_order / cancel_order and
//               send (or assemble the replay output line); stamp LAT / PROF.
//   supervisor  start the strategy process, watch it (waitpid + heartbeat),
//               and when it dies: cancel our resting orders (kill switch),
//               start a new one, and resync its positions.
// The gateway never trades on its own and never blocks on the strategy for
// market data: if the strategy is down or the ring is full, book updates are
// dropped and counted.
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "fast_codec.hpp"
#include "shm_layout.hpp"
#include "spin.hpp"

extern char** environ;

namespace {

using ipc::InMsg;
using ipc::InType;
using ipc::OutMsg;
using ipc::OutType;
using json = nlohmann::json;

constexpr std::int64_t kHeartbeatTimeoutNs = 500'000'000;  // strategy presumed hung after 500 ms
constexpr int kRestartDelayMs = 50;
constexpr int kMaxRestartsPerMinute = 20;

std::string env_or(const char* key, const std::string& def) {
  const char* v = std::getenv(key);
  return v && *v ? std::string(v) : def;
}

struct Config {
  std::string team_id, url, token, shm_name, strategy_path;
  int level = 1;
  bool replay = false, profile = false, spawn = true;
};

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

// What the gateway knows about our orders and positions, from the venue's own
// messages. Used for the kill switch and to resync a restarted strategy.
// Touched by rx, tx and supervisor threads, never on the book path.
class Tracker {
 public:
  struct Open {
    std::string id, symbol;
    char side;
  };
  void on_ack(std::string_view id, std::string_view symbol, char side) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& o : open_)
      if (o.id == id) return;
    open_.push_back({std::string(id), std::string(symbol), side});
  }
  void on_fill(std::string_view symbol, char side, int qty) {
    std::lock_guard<std::mutex> g(m_);
    position_ref(symbol) += side == 'b' ? qty : -qty;
    for (auto it = open_.begin(); it != open_.end(); ++it)
      if (it->symbol == symbol && it->side == side) {  // one-lot orders
        open_.erase(it);
        break;
      }
  }
  void on_cancel_sent(std::string_view id) {
    std::lock_guard<std::mutex> g(m_);
    open_.erase(std::remove_if(open_.begin(), open_.end(), [&](const Open& o) { return o.id == id; }),
                open_.end());
  }
  std::vector<Open> take_open() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<Open> out;
    out.swap(open_);
    return out;
  }
  std::vector<std::pair<std::string, int>> positions() {
    std::lock_guard<std::mutex> g(m_);
    return positions_;
  }

 private:
  int& position_ref(std::string_view symbol) {
    for (auto& p : positions_)
      if (p.first == symbol) return p.second;
    positions_.emplace_back(std::string(symbol), 0);
    return positions_.back().second;
  }
  std::mutex m_;
  std::vector<Open> open_;
  std::vector<std::pair<std::string, int>> positions_;
};

class Gateway {
 public:
  explicit Gateway(Config cfg) : cfg_(std::move(cfg)) {
    view_.symbol.reserve(128);
    line_.reserve(1 << 16);
    for (auto& s : wire_) s.reserve(512);
  }

  int run_live();
  int run_replay(std::istream& in, std::ostream& out);

 private:
  bool start();
  void finish();
  void ingest(const std::string& raw);
  template <class Fill>
  bool push_in(bool droppable, Fill&& fill);
  void push_resync();
  void decode_other(const std::string& raw);
  void tx_loop();
  void deliver(const OutMsg& m);
  void flush_through(std::uint64_t seq, bool seq_answered);
  void supervise_loop();
  bool spawn_strategy();
  void strategy_lost(const char* why);
  void send_ws(const std::string& s) { ws_.sendText(s); }

  Config cfg_;
  ipc::Mapping map_;
  ipc::Region* r_ = nullptr;
  ix::WebSocket ws_;
  Tracker tracker_;
  arena::FastCodec codec_;

  std::atomic<bool> running_{false};
  std::atomic<bool> strategy_up_{false};
  std::atomic<bool> resync_pending_{false};
  std::atomic<int> child_{-1};
  std::thread tx_thread_, sup_thread_;

  // rx thread
  arena::BookView view_;
  std::uint64_t seq_ = 0;
  long long frames_ = 0, books_pushed_ = 0, dropped_full_ = 0, dropped_down_ = 0;

  // tx thread
  std::array<std::string, 2> wire_;
  std::ostream* out_ = nullptr;
  std::string line_;
  std::atomic<std::uint64_t> next_line_{1};
  std::atomic<std::uint64_t> force_through_{0};
  long long orders_sent_ = 0, lost_lines_ = 0;
  hft::LatencyHist ipc_out_, tick_to_trade_;

  // supervisor
  std::atomic<long long> restarts_{0}, killswitch_cancels_{0};
  std::atomic<std::int64_t> last_recovery_ns_{0};
  std::atomic<bool> strategy_quit_{false};
};

// ── lifecycle ───────────────────────────────────────────────────────────────

bool Gateway::start() {
  if (!map_.create(cfg_.shm_name, cfg_.replay)) return false;
  r_ = map_.region();
  r_->ctl.gw_heartbeat_ns.store(hft::now_ns());
  running_.store(true);
  if (cfg_.spawn && !spawn_strategy()) return false;
  std::fprintf(stderr, "[gateway] shm %s (%zu KB); waiting for strategy%s\n", cfg_.shm_name.c_str(),
               sizeof(ipc::Region) / 1024, cfg_.spawn ? "" : " (start it with HFT_SHM set)");
  // Start handshake: no market data flows until a strategy is consuming.
  const std::int64_t deadline = hft::now_ns() + (cfg_.spawn ? 5'000'000'000LL : (1LL << 62));
  while (!r_->ctl.st_ready.load(std::memory_order_acquire)) {
    if (g_stop.load() || hft::now_ns() > deadline) {
      std::fprintf(stderr, "[gateway] strategy did not start\n");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  strategy_up_.store(true);
  tx_thread_ = std::thread([this] { tx_loop(); });
  sup_thread_ = std::thread([this] { supervise_loop(); });
  return true;
}

// Stop handshake: ask the strategy to exit, wait for it, then tear down.
void Gateway::finish() {
  r_->ctl.shutdown.store(1, std::memory_order_release);
  running_.store(false);
  if (sup_thread_.joinable()) sup_thread_.join();
  const int child = child_.exchange(-1);
  if (child > 0) {
    int st = 0;
    for (int i = 0; i < 200 && ::waitpid(child, &st, WNOHANG) == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (::waitpid(child, &st, WNOHANG) == 0) {
      ::kill(child, SIGKILL);
      ::waitpid(child, &st, 0);
    }
  }
  if (tx_thread_.joinable()) tx_thread_.join();
  std::fprintf(stderr,
               "[gateway] frames=%lld books_pushed=%lld dropped_ring_full=%lld "
               "dropped_strategy_down=%lld orders_sent=%lld lost_lines=%lld\n",
               frames_, books_pushed_, dropped_full_, dropped_down_, orders_sent_, lost_lines_);
  std::fprintf(stderr, "[gateway] strategy_restarts=%lld killswitch_cancels=%lld last_recovery_ms=%.1f\n",
               restarts_.load(), killswitch_cancels_.load(), last_recovery_ns_.load() / 1e6);
  ipc_out_.print("[ipc] out-ring one-way");
  tick_to_trade_.print("[gateway] tick-to-trade (decode -> order handed to socket/output)");
}

int Gateway::run_live() {
  ws_.setUrl(cfg_.url);
  ws_.setPingInterval(15);
  ws_.setMaxWaitBetweenReconnectionRetries(3000);
  ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open: {
        std::fprintf(stderr, "[gateway] connected to %s as %s\n", cfg_.url.c_str(), cfg_.team_id.c_str());
        r_->ctl.session_open.store(0);
        json hs = {{"type", "handshake"}, {"team_id", cfg_.team_id}, {"role", "trader"},
                   {"level", cfg_.level},  {"token", cfg_.token}};
        send_ws(hs.dump());
        break;
      }
      case ix::WebSocketMessageType::Message:
        ingest(msg->str);
        break;
      case ix::WebSocketMessageType::Close:
        std::fprintf(stderr, "[gateway] connection closed (%d) — reconnecting\n", msg->closeInfo.code);
        r_->ctl.session_open.store(0);
        break;
      case ix::WebSocketMessageType::Error:
        std::fprintf(stderr, "[gateway] socket error: %s\n", msg->errorInfo.reason.c_str());
        break;
      default:
        break;
    }
  });
  if (!start()) return 1;
  ws_.start();
  while (!g_stop.load() && !strategy_quit_.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ws_.stop();
  finish();
  return 0;
}

int Gateway::run_replay(std::istream& in, std::ostream& out) {
  out_ = &out;
  if (!start()) return 1;
  std::string raw;
  raw.reserve(1 << 20);
  while (!g_stop.load() && std::getline(in, raw)) ingest(raw);
  // Wait for every line to be answered. A line lost with a crashed strategy is
  // answered "[]" when a later line completes; if the very last one was lost,
  // force it out once the pipeline has gone quiet.
  std::uint64_t seen = 0;
  std::int64_t quiet_since = hft::now_ns();
  while (next_line_.load() <= seq_ && !g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::uint64_t now_line = next_line_.load();
    if (now_line != seen) {
      seen = now_line;
      quiet_since = hft::now_ns();
    } else if (hft::now_ns() - quiet_since > 2'000'000'000 && r_->in.empty() &&
               strategy_up_.load()) {
      force_through_.store(seq_);
    }
  }
  finish();
  return 0;
}

// ── rx thread ───────────────────────────────────────────────────────────────

// Claim an inbound slot, fill it in place, publish it. Droppable messages (book
// updates) are dropped when the ring is full; others wait, unless no strategy
// is running in live mode (its successor is resynced instead).
template <class Fill>
bool Gateway::push_in(bool droppable, Fill&& fill) {
  hft::Backoff b;
  InMsg* slot;
  while (!(slot = r_->in.claim())) {
    if (droppable) {
      ++dropped_full_;
      return false;
    }
    if ((!cfg_.replay && !strategy_up_.load()) || !running_.load()) return false;
    b.idle();
  }
  fill(*slot);
  slot->t_push_ns = hft::now_ns();
  r_->in.commit();
  return true;
}

void Gateway::push_resync() {
  for (const auto& p : tracker_.positions())
    push_in(false, [&](InMsg& m) {
      m.seq = 0;
      m.t_recv_ns = hft::now_ns();
      m.type = InType::kResync;
      ipc::put(m.symbol, p.first);
      m.position = p.second;
    });
}

void Gateway::ingest(const std::string& raw) {
  const std::int64_t t_recv = hft::now_ns();
  ++frames_;
  const std::uint64_t seq = ++seq_;  // every frame counts, so dropped books show as gaps
  if (resync_pending_.exchange(false)) push_resync();

  const bool up = cfg_.replay || strategy_up_.load(std::memory_order_acquire);
  if (codec_.decode(raw, view_)) {
    if (!up) {
      ++dropped_down_;
      return;
    }
    const bool pushed = push_in(!cfg_.replay, [&](InMsg& m) {
      m.seq = seq;
      m.t_recv_ns = t_recv;
      m.type = InType::kBook;
      ipc::put(m.symbol, view_.symbol);
      m.best_bid = view_.best_bid;
      m.best_ask = view_.best_ask;
      m.mid = view_.mid;
      m.spread = view_.spread;
      m.microprice = view_.microprice;
      m.obi = view_.obi;
      // Keep the kShmDepth best levels per side (all of them when it fits).
      const auto by_bid = [](const arena::Level& a, const arena::Level& b) { return a.price > b.price; };
      const auto by_ask = [](const arena::Level& a, const arena::Level& b) { return a.price < b.price; };
      if (view_.bid_count <= ipc::kShmDepth) {
        std::copy(view_.bids.begin(), view_.bids.begin() + view_.bid_count, m.bids);
        m.bid_count = static_cast<std::uint8_t>(view_.bid_count);
      } else {
        std::partial_sort_copy(view_.bids.begin(), view_.bids.begin() + view_.bid_count, m.bids,
                               m.bids + ipc::kShmDepth, by_bid);
        m.bid_count = ipc::kShmDepth;
      }
      if (view_.ask_count <= ipc::kShmDepth) {
        std::copy(view_.asks.begin(), view_.asks.begin() + view_.ask_count, m.asks);
        m.ask_count = static_cast<std::uint8_t>(view_.ask_count);
      } else {
        std::partial_sort_copy(view_.asks.begin(), view_.asks.begin() + view_.ask_count, m.asks,
                               m.asks + ipc::kShmDepth, by_ask);
        m.ask_count = ipc::kShmDepth;
      }
    });
    books_pushed_ += pushed;
    return;
  }
  decode_other(raw);
}

// Acks, fills, errors and session events: rarer, parsed with the DOM.
void Gateway::decode_other(const std::string& raw) {
  const std::uint64_t seq = seq_;
  const std::int64_t t_recv = hft::now_ns();
  json m;
  std::string type;
  if (!raw.empty()) {
    try {
      m = json::parse(raw);
      type = m.value("type", "");
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[gateway] bad JSON frame: %s\n", e.what());
    }
  }
  const auto side_of = [](const std::string& s) { return s == "buy" ? 'b' : s == "sell" ? 's' : '\0'; };
  InType kind = InType::kIgnored;
  std::string symbol, id, code, text;
  char side = 0;
  int qty = 0, ahead = -1, level_qty = 0;

  if (type == "order_ack" || type == "queue_update") {
    kind = type == "order_ack" ? InType::kAck : InType::kQueue;
    id = m.value("order_id", "");
    symbol = m.value("symbol", "");
    side = side_of(m.value("side", ""));
    qty = m.value("quantity", 0);
    ahead = m.value("queue_ahead", -1);
    level_qty = m.value("level_qty", 0);
    if (kind == InType::kAck) tracker_.on_ack(id, symbol, side);
  } else if (type == "trade_execution") {
    symbol = m.value("symbol", "");
    qty = m.value("quantity", 0);
    if (m.value("buyer_id", "") == cfg_.team_id) side = 'b';
    else if (m.value("seller_id", "") == cfg_.team_id) side = 's';
    if (side) {
      kind = InType::kFill;
      tracker_.on_fill(symbol, side, qty);
    }
  } else if (type == "session_event") {
    const std::string ev = m.value("event", "");
    if (ev == "SESSION_OPEN" || ev == "SESSION_PREOPEN") r_->ctl.session_open.store(1);
    else if (ev == "SESSION_CLOSED") r_->ctl.session_open.store(0);
  } else if (type == "error") {
    code = m.value("code", "");
    text = m.value("message", "");
    std::fprintf(stderr, "[gateway] server error %s: %s\n", code.c_str(), text.c_str());
    if (code.rfind("AUTH", 0) == 0) g_stop.store(true);  // configuration problem: stop everything
    kind = InType::kError;
  } else if (type == "book_snapshot") {
    std::fprintf(stderr, "[gateway] malformed book_snapshot dropped\n");
  }

  if (kind == InType::kIgnored && !cfg_.replay) return;  // nothing for the strategy
  if (!cfg_.replay && !strategy_up_.load()) return;      // its successor gets a resync
  push_in(false, [&](InMsg& msg) {
    msg.seq = seq;
    msg.t_recv_ns = t_recv;
    msg.type = kind;
    msg.side = side;
    msg.quantity = qty;
    msg.queue_ahead = ahead;
    msg.level_qty = level_qty;
    msg.bid_count = msg.ask_count = 0;
    ipc::put(msg.symbol, symbol);
    ipc::put(msg.order_id, id);
    ipc::put(msg.code, code);
    ipc::put(msg.text, text);
  });
}

// ── tx thread ───────────────────────────────────────────────────────────────

void Gateway::tx_loop() {
  hft::Backoff b;
  OutMsg m;
  while (true) {
    if (r_->out.pop(m)) {
      b.reset();
      deliver(m);
      continue;
    }
    if (const std::uint64_t f = force_through_.exchange(0)) flush_through(f, false);
    if (!running_.load(std::memory_order_acquire) && r_->out.empty()) break;
    b.idle();
  }
}

void Gateway::deliver(const OutMsg& m) {
  const std::int64_t t_pop = hft::now_ns();
  if (m.type == OutType::kDone) {
    flush_through(m.seq, true);
    return;
  }
  ipc_out_.add(t_pop - m.t_out_push_ns);
  std::string& wire = wire_[0];
  if (m.type == OutType::kPlace) {
    arena::PlaceOrder o;
    o.symbol.set(ipc::get(m.symbol));
    o.buy = m.side == 'b';
    o.quantity = m.quantity;
    o.price = m.price;
    o.market = m.market;
    codec_.encode(o, cfg_.team_id, wire);
  } else {
    arena::CancelOrder o;
    o.symbol.set(ipc::get(m.symbol));
    o.order_id.set(ipc::get(m.order_id));
    codec_.encode(o, cfg_.team_id, wire);
    tracker_.on_cancel_sent(ipc::get(m.order_id));
  }
  if (cfg_.replay) {
    if (m.seq < next_line_.load()) return;  // belongs to a line already answered
    if (m.seq > next_line_.load()) flush_through(m.seq - 1, false);
    if (line_.size() > 1) line_ += ',';
    else line_ = "[";
    line_ += wire;
  } else {
    send_ws(wire);
  }
  const std::int64_t t_sent = hft::now_ns();
  ++orders_sent_;
  tick_to_trade_.add(t_sent - m.t_recv_ns);
  std::fprintf(stderr, "LAT %s %.3f\n", m.symbol, (t_sent - m.t_recv_ns) / 1000.0);
  if (cfg_.profile)  // rx decode | in-ring | strategy | out-ring | encode+send | total (ns)
    std::fprintf(stderr, "PROF %lld %lld %lld %lld %lld %lld\n",
                 static_cast<long long>(m.t_in_push_ns - m.t_recv_ns),
                 static_cast<long long>(m.t_in_pop_ns - m.t_in_push_ns),
                 static_cast<long long>(m.t_out_push_ns - m.t_in_pop_ns),
                 static_cast<long long>(t_pop - m.t_out_push_ns),
                 static_cast<long long>(t_sent - t_pop), static_cast<long long>(t_sent - m.t_recv_ns));
}

// Replay: write the output lines up to and including `seq`. The pending order
// list belongs to the first of them. Lines without their own "done" (all but
// `seq`, and `seq` too unless `seq_answered`) were lost with a crashed strategy
// and are answered with whatever was collected, usually "[]".
void Gateway::flush_through(std::uint64_t seq, bool seq_answered) {
  std::uint64_t next = next_line_.load();
  if (!out_ || seq < next) return;
  for (; next <= seq; ++next) {
    if (next < seq || !seq_answered) ++lost_lines_;
    if (line_.empty()) line_ = "[";
    line_ += "]\n";
    out_->write(line_.data(), static_cast<std::streamsize>(line_.size()));
    line_.clear();
  }
  out_->flush();
  next_line_.store(next);
}

// ── supervisor thread ───────────────────────────────────────────────────────

bool Gateway::spawn_strategy() {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);  // stdout is the replay channel
  char* argv[] = {const_cast<char*>(cfg_.strategy_path.c_str()), nullptr};
  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, cfg_.strategy_path.c_str(), &fa, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    std::fprintf(stderr, "[gateway] cannot start %s: %s\n", cfg_.strategy_path.c_str(), std::strerror(rc));
    return false;
  }
  child_.store(pid);
  return true;
}

// The strategy is gone: stop feeding it, pull our resting orders, free the
// consumer slot for its successor.
void Gateway::strategy_lost(const char* why) {
  strategy_up_.store(false);
  ipc::Control& c = r_->ctl;
  c.st_ready.store(0);
  c.st_pid.store(0);
  const auto open = tracker_.take_open();
  std::fprintf(stderr, "[gateway] strategy lost (%s); cancelling %zu resting orders\n", why, open.size());
  if (!cfg_.replay) {
    arena::FastCodec codec;
    std::string wire;
    for (const auto& o : open) {
      arena::CancelOrder msg;
      msg.symbol.set(o.symbol);
      msg.order_id.set(o.id);
      codec.encode(msg, cfg_.team_id, wire);
      send_ws(wire);
      killswitch_cancels_.fetch_add(1);
    }
  }
}

void Gateway::supervise_loop() {
  ipc::Control& c = r_->ctl;
  std::uint32_t epoch = c.st_epoch.load();
  std::int64_t lost_at = 0, restart_at = 0;
  std::vector<std::int64_t> restarts;
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::int64_t now = hft::now_ns();
    c.gw_heartbeat_ns.store(now, std::memory_order_relaxed);
    if (!running_.load() || c.shutdown.load()) break;

    if (strategy_up_.load()) {
      // 1. exited (we started it), 2. hung (stale heartbeat), 3. gone (external)
      const int child = child_.load();
      int st = 0;
      const char* why = nullptr;
      if (child > 0 && ::waitpid(child, &st, WNOHANG) == child) {
        child_.store(-1);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 3) {
          std::fprintf(stderr, "[gateway] strategy asked to stop; shutting down\n");
          strategy_quit_.store(true);
          g_stop.store(true);
          break;
        }
        why = WIFSIGNALED(st) ? "killed by signal" : "exited";
      } else if (now - c.st_heartbeat_ns.load() > kHeartbeatTimeoutNs) {
        why = "heartbeat timeout";
        if (child > 0) {
          ::kill(child, SIGKILL);
          ::waitpid(child, &st, 0);
          child_.store(-1);
        }
      } else if (child <= 0 && !ipc::pid_alive(c.st_pid.load())) {
        why = "process gone";
      }
      if (why) {
        strategy_lost(why);
        lost_at = now;
        restart_at = now + kRestartDelayMs * 1'000'000LL;
      }
      continue;
    }

    // Down: restart it (if we own it), then wait for the new epoch.
    if (cfg_.spawn && child_.load() <= 0 && restart_at && now >= restart_at) {
      restarts.erase(std::remove_if(restarts.begin(), restarts.end(),
                                    [&](std::int64_t t) { return now - t > 60'000'000'000LL; }),
                     restarts.end());
      if (static_cast<int>(restarts.size()) >= kMaxRestartsPerMinute) {
        std::fprintf(stderr, "[gateway] strategy keeps crashing; giving up\n");
        g_stop.store(true);
        break;
      }
      restarts.push_back(now);
      restart_at = 0;
      spawn_strategy();
    }
    const std::uint32_t e = c.st_epoch.load();
    if (e != epoch && c.st_ready.load(std::memory_order_acquire)) {
      epoch = e;
      resync_pending_.store(true);
      strategy_up_.store(true, std::memory_order_release);
      if (lost_at) {
        restarts_.fetch_add(1);
        last_recovery_ns_.store(now - lost_at);
        std::fprintf(stderr, "[gateway] strategy back (epoch %u) %.1f ms after the loss\n", e,
                     (now - lost_at) / 1e6);
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  cfg.team_id = env_or("TEAM_ID", "hft_alpha");
  cfg.token = env_or("ARENA_TOKEN", "");
  const std::string url = env_or("EXCHANGE_URL", "");
  cfg.url = !url.empty() ? url
                         : "ws://" + env_or("EXCHANGE_HOST", "localhost") + ":" + env_or("EXCHANGE_PORT", "8765");
  cfg.level = std::atoi(env_or("ARENA_LEVEL", "1").c_str());
  // The strategy binary sits next to this one unless STRATEGY_BIN says otherwise.
  std::string self = argv[0];
  const std::size_t slash = self.find_last_of('/');
  cfg.strategy_path = env_or("STRATEGY_BIN", slash == std::string::npos ? "./strategy"
                                                                        : self.substr(0, slash) + "/strategy");
  cfg.shm_name = env_or("HFT_SHM", "/hft16_" + std::to_string(::getpid()));
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--replay") cfg.replay = true;
    else if (a == "--profile") cfg.profile = true;
    else if (a == "--no-spawn") cfg.spawn = false;
  }
  ::setenv("HFT_SHM", cfg.shm_name.c_str(), 1);  // inherited by the strategy we start
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  Gateway gw(cfg);
  return cfg.replay ? gw.run_replay(std::cin, std::cout) : gw.run_live();
}
