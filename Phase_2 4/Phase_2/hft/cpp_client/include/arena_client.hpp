// arena_client.hpp — AlgoArena HFT client transport layer (Phase 2).
//
// ArenaClient owns the WebSocket to the Python exchange, decodes inbound
// frames, and serializes orders. Book snapshots take the allocation-free path
// (FastCodec SAX decode -> on_book_snapshot -> per-tick message arena ->
// encode + send). Everything else (acks, fills, session events) is rarer and
// uses the nlohmann DOM parser.
//
// Tick-to-order latency is stamped from the instant a frame reaches
// dispatch() to the moment each order of that tick has been handed to the
// socket. LAT lines (the course latency-log contract) are written only after
// every order of the tick has been sent, so logging never delays a send.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

#include <ixwebsocket/IXWebSocket.h>

#include "fast_codec.hpp"

namespace arena {

struct ClientConfig {
  std::string team_id;  // TEAM_ID (default "hft_alpha")
  std::string url;      // EXCHANGE_URL, else ws://EXCHANGE_HOST:EXCHANGE_PORT
  std::string token;    // ARENA_TOKEN
  bool profile = false; // --profile: per-tick PROF lines on stderr
  double tick_size = 0.01;  // TICK_SIZE
  int level = 1;            // ARENA_LEVEL
  static ClientConfig from_env();
};

class ArenaClient {
 public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;

  static constexpr std::size_t kMaxSymbols = 64;

  explicit ArenaClient(ClientConfig cfg);
  virtual ~ArenaClient();
  ArenaClient(const ArenaClient&) = delete;
  ArenaClient& operator=(const ArenaClient&) = delete;

  // Connect and service the socket until stop(); reconnects on drop.
  void run();
  // Offline mode: one JSON message per stdin line, one JSON array of the
  // orders it produced per stdout line (course replay protocol).
  void run_replay(std::istream& in, std::ostream& out);
  // Safe from a signal handler or another thread.
  void stop();

  void place_limit(const std::string& symbol, const std::string& side, int quantity,
                   double price);
  void place_market(const std::string& symbol, const std::string& side, int quantity);
  void cancel(const std::string& order_id, const std::string& symbol);

  const std::string& team_id() const { return cfg_.team_id; }
  const ClientConfig& config() const { return cfg_; }
  bool session_open() const { return session_open_.load(); }
  // book_snapshot frames the decoder refused (never reach on_depth).
  long long malformed_books() const { return malformed_books_; }
  // Latest venue top-of-book for a symbol (scalars only; depth is not kept).
  bool book(const std::string& symbol, BookView& out) const;

 protected:
  // Orders queued between begin_tick and end_tick are sent together at
  // end_tick and stamped against `received`.
  void begin_tick(const std::string& symbol, time_point received);
  void end_tick();

  virtual void on_book_snapshot(const BookView&, time_point) {}
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
  // Called once when run() or run_replay() returns (print summaries here).
  virtual void on_stop() {}

 private:
  // Venue top-of-book cache behind book(); depth is deliberately not copied.
  struct Top {
    std::string symbol;
    double best_bid = 0, best_ask = 0, mid = 0, spread = 0, ref = 0, microprice = 0, obi = 0;
  };

  void connect_handlers();
  void send_handshake();
  void dispatch(const std::string& raw);
  void handle_book(time_point recv_time);
  void store_top(const BookView& b);
  void dispatch_other(const std::string& raw);
  void send_raw(const std::string& payload);
  void place_order(const std::string& symbol, const std::string& side, int quantity,
                   double price, bool market);
  void queue(const Message& message);
  void flush_orders();
  void log_latencies(std::size_t n);

  ClientConfig cfg_;

  // ── hot path state (receive thread only) ──────────────────────────────
  FastCodec codec_;
  BookView snapshot_;
  MessageArena messages_;
  std::array<std::string, 16> output_;
  std::array<long long, 16> tick_lat_ns_{};
  bool in_tick_ = false;
  const std::string* tick_symbol_ = nullptr;
  time_point tick_time_{};
  long long serialize_ns_ = 0;
  long long malformed_books_ = 0;

  // ── top-of-book cache (book() may be called from another thread) ─────
  mutable std::mutex book_mtx_;
  std::array<Top, kMaxSymbols> tops_;
  std::size_t top_count_ = 0;

  // ── latency summary (receive thread only) ──────────────────────────────
  long long lat_count_ = 0, lat_sum_ns_ = 0, lat_min_ns_ = 0, lat_max_ns_ = 0;
  char log_buf_[16 * 160];

  // ── transport / replay ─────────────────────────────────────────────────
  ix::WebSocket ws_;
  std::atomic<bool> session_open_{false};
  std::atomic<bool> running_{false};
  bool replay_mode_ = false;
  std::array<std::string, 16> replay_output_;
  std::size_t replay_count_ = 0;
};

}  // namespace arena
