// arena_client.hpp — AlgoArena HFT client, Phase 3 threaded pipeline.
//
// Three threads, connected by lock-free SPSC rings of slot indices
// (see pipeline.hpp):
//
//   rx       socket callback (live) or stdin reader (replay): stamp, decode
//            into a free Event slot, push its index to ticks_.
//   strategy pop an Event, run the bot's hooks, encode the orders into a free
//            OutSlot, push its index to sends_, hand the Event slot back.
//   send     pop an OutSlot, write each message to the socket (or the replay
//            line), stamp LAT, then log. Logging lives here, off the decision
//            path. SEND_THREAD=0 runs this stage inline on the strategy thread.
//
// The strategy thread never takes a lock, never allocates, and never makes a
// system call while there is work; it only spins (then yields) when idle.
#pragma once
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <ixwebsocket/IXWebSocket.h>

#include "fast_codec.hpp"
#include "pipeline.hpp"
#include "quota.hpp"
#include "spsc_ring.hpp"

namespace arena {

struct ClientConfig {
  std::string team_id;   // TEAM_ID (default "hft_alpha")
  std::string url;       // EXCHANGE_URL, else ws://EXCHANGE_HOST:EXCHANGE_PORT
  std::string token;     // ARENA_TOKEN
  bool profile = false;  // --profile: per-event PROF lines on stderr
  double tick_size = 0.01;  // TICK_SIZE
  int level = 1;            // ARENA_LEVEL
  bool send_thread = true;  // SEND_THREAD (0 = send inline on the strategy thread)
  std::array<int, 3> cores{-1, -1, -1};  // PIN_CORES="rx,strategy,send" (Linux)
  int order_quota = 6;                   // ORDER_QUOTA messages per window (0 = off)
  long long quota_window_ms = 1000;      // ORDER_QUOTA_MS
  static ClientConfig from_env();
};

class ArenaClient {
 public:
  using clock = Clock;
  using time_point = TimePoint;

  explicit ArenaClient(ClientConfig cfg);
  virtual ~ArenaClient();
  ArenaClient(const ArenaClient&) = delete;
  ArenaClient& operator=(const ArenaClient&) = delete;

  // Connect and trade until stop(); reconnects on drop.
  void run();
  // Offline: one JSON message per stdin line in, one JSON array of the orders
  // it produced per stdout line out, in order (course replay protocol).
  void run_replay(std::istream& in, std::ostream& out);
  // Safe from any thread or a signal handler.
  void stop();

  const std::string& team_id() const { return cfg_.team_id; }
  const ClientConfig& config() const { return cfg_; }
  bool session_open() const { return session_open_.load(std::memory_order_acquire); }

 protected:
  // ── strategy-thread API ─────────────────────────────────────────────────
  void place_limit(const std::string& symbol, const std::string& side, int quantity,
                   double price);
  void place_market(const std::string& symbol, const std::string& side, int quantity);
  void cancel(const std::string& order_id, const std::string& symbol);
  // Spend one message of the order_quota budget (cancels may use the reserve).
  bool quota_allows(bool cancel) { return quota_.allow(event_ns_, cancel); }
  const QuotaGuard& quota() const { return quota_; }
  long long malformed_books() const { return malformed_books_.load(std::memory_order_relaxed); }

  // ── hooks, all called on the strategy thread ────────────────────────────
  virtual void on_book_snapshot(const BookView&) {}
  virtual void on_fill(const std::string& /*side*/, const std::string& /*symbol*/,
                       int /*quantity*/, double /*price*/, bool /*maker*/) {}
  virtual void on_portfolio(double, double, double, double) {}
  virtual void on_session(const std::string& /*event*/, const std::string& /*message*/) {}
  virtual void on_order_ack(const std::string& /*order_id*/, const std::string& /*symbol*/,
                            const std::string& /*side*/, double /*price*/, int /*quantity*/,
                            int /*queue_ahead*/, int /*level_qty*/) {}
  virtual void on_queue_update(const std::string& /*order_id*/, const std::string& /*symbol*/,
                               const std::string& /*side*/, double /*price*/,
                               int /*queue_ahead*/, int /*level_qty*/) {}
  virtual void on_error(const std::string& /*code*/, const std::string& /*message*/) {}
  // Once, after every thread has stopped (print summaries here).
  virtual void on_stop() {}

 private:
  // rx thread
  void connect_handlers();
  void send_handshake();
  void ingest(const std::string& raw, bool replay);
  bool acquire_in_slot(std::uint64_t& slot, const std::string& raw, bool replay);
  void decode_other(const std::string& raw, Event& e);
  // strategy thread
  void strategy_loop();
  void handle(Event& e, OutSlot& out);
  void queue(const Message& m);
  // send thread
  void send_loop();
  void deliver(OutSlot& out);
  void log_slot(const OutSlot& out, time_point send_start, time_point send_end);

  void start_workers();
  void join_workers();
  void print_pipeline_stats() const;

  ClientConfig cfg_;

  // ── pools and rings (fixed at construction) ─────────────────────────────
  std::unique_ptr<Event[]> in_;
  std::unique_ptr<OutSlot[]> out_;
  SPSCRing ticks_{kInSlots};      // rx -> strategy
  SPSCRing free_in_{kInSlots};    // strategy -> rx
  SPSCRing sends_{kOutSlots};     // strategy -> send
  SPSCRing free_out_{kOutSlots};  // send -> strategy

  // ── rx-thread state ──────────────────────────────────────────────────────
  FastCodec rx_codec_;
  std::uint64_t spare_in_ = ~0ULL;  // slot taken but not pushed (frame ignored)
  long long produced_ = 0, max_backlog_ = 0;
  std::atomic<long long> dropped_books_{0};
  std::atomic<long long> malformed_books_{0};
  bool rx_pinned_ = false;

  // ── strategy-thread state ───────────────────────────────────────────────
  FastCodec tx_codec_;
  MessageArena messages_;
  QuotaGuard quota_;
  long long event_ns_ = 0;
  std::atomic<long long> consumed_{0};
  std::atomic<bool> strategy_done_{false};

  // ── send-thread state ────────────────────────────────────────────────────
  std::ostream* replay_out_ = nullptr;
  std::string line_;
  char log_buf_[kMaxTickMessages * 160];
  long long lat_count_ = 0, lat_sum_ns_ = 0, lat_max_ns_ = 0;

  // ── shared ──────────────────────────────────────────────────────────────
  ix::WebSocket ws_;
  std::atomic<bool> running_{false};
  std::atomic<bool> session_open_{false};
  bool replay_mode_ = false;
  std::thread strategy_thread_, send_thread_;
};

}  // namespace arena
