// arena_client.cpp — implementation of the AlgoArena HFT transport layer.
// Outbound JSON matches shared/messages.py (PlaceOrder / CancelOrder /
// Handshake). See arena_client.hpp for the hot-path contract.
#include "arena_client.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace arena {
namespace {

std::string env_or(const char* key, const std::string& def) {
  const char* v = std::getenv(key);
  if (v == nullptr || *v == '\0') return def;
  return std::string(v);
}

long long ns_since(ArenaClient::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(ArenaClient::clock::now() - t)
      .count();
}

}  // namespace

ClientConfig ClientConfig::from_env() {
  ClientConfig c;
  c.team_id = env_or("TEAM_ID", "hft_alpha");
  c.token = env_or("ARENA_TOKEN", "");
  try {
    c.tick_size = std::stod(env_or("TICK_SIZE", "0.01"));
  } catch (...) {
    c.tick_size = 0;  // rejected by the strategy constructor
  }
  const std::string url = env_or("EXCHANGE_URL", "");
  if (!url.empty()) {
    c.url = url;
  } else {
    c.url = "ws://" + env_or("EXCHANGE_HOST", "localhost") + ":" +
            env_or("EXCHANGE_PORT", "8765");
  }
  try {
    c.level = std::stoi(env_or("ARENA_LEVEL", "1"));
  } catch (...) {
    c.level = 1;
  }
  return c;
}

ArenaClient::ArenaClient(ClientConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.team_id.size() > 128) throw std::length_error("team_id exceeds 128 bytes");
  // Reserve everything the hot path writes, so it never allocates.
  snapshot_.symbol.reserve(128);
  for (auto& t : tops_) t.symbol.reserve(128);
  for (auto& out : output_) out.reserve(1024);
  for (auto& out : replay_output_) out.reserve(1024);
  ws_.setUrl(cfg_.url);
  ws_.setPingInterval(15);
  ws_.setMaxWaitBetweenReconnectionRetries(3000);
  connect_handlers();
}

ArenaClient::~ArenaClient() { ws_.stop(); }

void ArenaClient::connect_handlers() {
  ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        std::cerr << "[arena] connected to " << cfg_.url << " as " << cfg_.team_id
                  << " (trader)\n";
        session_open_.store(false);  // re-gate on every (re)connect
        send_handshake();
        break;
      case ix::WebSocketMessageType::Message:
        dispatch(msg->str);
        break;
      case ix::WebSocketMessageType::Close:
        std::cerr << "[arena] connection closed (" << msg->closeInfo.code << ": "
                  << msg->closeInfo.reason << ") — reconnecting\n";
        session_open_.store(false);
        break;
      case ix::WebSocketMessageType::Error:
        std::cerr << "[arena] socket error: " << msg->errorInfo.reason << " — retrying\n";
        break;
      default:
        break;
    }
  });
}

void ArenaClient::send_handshake() {
  json hs = {{"type", "handshake"}, {"team_id", cfg_.team_id}, {"role", "trader"},
             {"level", cfg_.level},  {"token", cfg_.token}};
  send_raw(hs.dump());
}

void ArenaClient::send_raw(const std::string& payload) {
  if (replay_mode_) {
    if (replay_count_ == replay_output_.size())
      throw std::runtime_error("replay message capacity exceeded");
    replay_output_[replay_count_++] = payload;
    return;
  }
  ws_.sendText(payload);
}

void ArenaClient::run() {
  running_.store(true);
  ws_.start();
  while (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ws_.stop();
  on_stop();
}

void ArenaClient::stop() { running_.store(false); }

void ArenaClient::run_replay(std::istream& in, std::ostream& out) {
  replay_mode_ = true;
  session_open_.store(true);
  std::string line;
  line.reserve(1 << 20);
  while (std::getline(in, line)) {
    replay_count_ = 0;
    if (!line.empty()) dispatch(line);
    out << '[';
    for (std::size_t i = 0; i < replay_count_; ++i) {
      if (i) out << ',';
      out << replay_output_[i];
    }
    out << "]\n";
    out.flush();
  }
  on_stop();
}

// ── Inbound ─────────────────────────────────────────────────────────────────

void ArenaClient::dispatch(const std::string& raw) {
  const time_point recv_time = clock::now();
  // The SAX decoder stops at "type" for non-snapshots, so trying it first
  // costs little for acks/fills and keeps snapshots off the DOM path.
  if (codec_.decode(raw, snapshot_)) {
    handle_book(recv_time);
    return;
  }
  dispatch_other(raw);
}

void ArenaClient::handle_book(time_point recv_time) {
  store_top(snapshot_);
  if (!cfg_.profile) {
    on_book_snapshot(snapshot_, recv_time);
    return;
  }
  const long long decode_ns = ns_since(recv_time);
  serialize_ns_ = 0;
  const time_point start = clock::now();
  on_book_snapshot(snapshot_, recv_time);
  const long long hot_ns = ns_since(start);
  // decode | book update + signal + strategy + encode + send | encode only
  std::fprintf(stderr, "PROF %lld %lld %lld\n", decode_ns, hot_ns, serialize_ns_);
}

// Copies scalars only (~70 bytes). Copying the whole BookView here would move
// 8 KB of depth per snapshot.
void ArenaClient::store_top(const BookView& b) {
  std::lock_guard<std::mutex> lock(book_mtx_);
  std::size_t i = 0;
  while (i < top_count_ && tops_[i].symbol != b.symbol) ++i;
  if (i == top_count_) {
    if (top_count_ == tops_.size()) return;
    tops_[top_count_++].symbol = b.symbol;
  }
  Top& t = tops_[i];
  t.best_bid = b.best_bid;
  t.best_ask = b.best_ask;
  t.mid = b.mid;
  t.spread = b.spread;
  t.ref = b.ref;
  t.microprice = b.microprice;
  t.obi = b.obi;
}

void ArenaClient::dispatch_other(const std::string& raw) {
  json m;
  try {
    m = json::parse(raw);
  } catch (const std::exception& e) {
    std::cerr << "[arena] bad JSON frame: " << e.what() << "\n";
    return;
  }
  const std::string type = m.value("type", "");
  if (type == "book_snapshot") {
    // Counted, and only the first few printed, so a bad feed cannot flood stderr.
    if (++malformed_books_ <= 3)
      std::cerr << "[arena] malformed book_snapshot dropped: " << raw.substr(0, 300) << "\n";
  } else if (type == "trade_execution") {
    const std::string buyer = m.value("buyer_id", "");
    const std::string seller = m.value("seller_id", "");
    std::string side;
    if (buyer == cfg_.team_id) side = "buy";
    else if (seller == cfg_.team_id) side = "sell";
    const bool maker = m.value("maker_id", "") == cfg_.team_id;
    on_fill(side, m.value("symbol", ""), m.value("quantity", 0), m.value("price", 0.0), maker);
  } else if (type == "portfolio_update") {
    on_portfolio(m.value("cash", 0.0), m.value("net_worth", 0.0), m.value("realized_pnl", 0.0),
                 m.value("unrealized_pnl", 0.0));
  } else if (type == "session_event") {
    const std::string ev = m.value("event", "");
    if (ev == "SESSION_OPEN" || ev == "SESSION_PREOPEN") session_open_.store(true);
    else if (ev == "SESSION_CLOSED") session_open_.store(false);
    on_session(ev, m.value("message", ""));
  } else if (type == "order_ack") {
    on_order_ack(m.value("order_id", ""), m.value("symbol", ""), m.value("side", ""),
                 m.value("price", 0.0), m.value("quantity", 0), m.value("queue_ahead", -1),
                 m.value("level_qty", 0));
  } else if (type == "queue_update") {
    on_queue_update(m.value("order_id", ""), m.value("symbol", ""), m.value("side", ""),
                    m.value("price", 0.0), m.value("queue_ahead", -1),
                    m.value("level_qty", 0));
  } else if (type == "error") {
    std::cerr << "[arena] server error " << m.value("code", "") << ": "
              << m.value("message", "") << "\n";
    on_error(m.value("code", ""), m.value("message", ""));
  }
}

// ── Outbound ────────────────────────────────────────────────────────────────

void ArenaClient::place_limit(const std::string& symbol, const std::string& side,
                              int quantity, double price) {
  place_order(symbol, side, quantity, price, false);
}

void ArenaClient::place_market(const std::string& symbol, const std::string& side,
                               int quantity) {
  place_order(symbol, side, quantity, 0, true);
}

void ArenaClient::place_order(const std::string& symbol, const std::string& side, int quantity,
                              double price, bool market) {
  PlaceOrder order;
  if (!order.symbol.set(symbol)) return;
  order.buy = side == "buy";
  order.quantity = quantity;
  order.price = price;
  order.market = market;
  queue(order);
}

void ArenaClient::cancel(const std::string& order_id, const std::string& symbol) {
  CancelOrder order;
  if (!order.symbol.set(symbol) || !order.order_id.set(order_id)) return;
  queue(order);
}

void ArenaClient::begin_tick(const std::string& symbol, time_point received) {
  messages_.reset();
  tick_symbol_ = &symbol;
  tick_time_ = received;
  in_tick_ = true;
}

void ArenaClient::queue(const Message& message) {
  Message* slot = messages_.alloc();
  if (!slot) return;  // more than 16 messages in one tick: drop, never allocate
  *slot = message;
  if (!in_tick_) flush_orders();  // sent from an ack/fill callback: no tick to stamp
}

void ArenaClient::end_tick() {
  flush_orders();
  in_tick_ = false;
}

void ArenaClient::flush_orders() {
  const std::size_t n = messages_.offset;
  for (std::size_t i = 0; i < n; ++i) {
    const time_point start = cfg_.profile ? clock::now() : time_point{};
    codec_.encode(messages_.slots[i], cfg_.team_id, output_[i]);
    if (cfg_.profile) serialize_ns_ += ns_since(start);
    send_raw(output_[i]);
    if (in_tick_) tick_lat_ns_[i] = ns_since(tick_time_);
  }
  messages_.reset();
  if (in_tick_ && n) log_latencies(n);  // after every order of the tick is out
}

// One fwrite per tick: "LAT <symbol> <micros>" per order (latency_report.py
// contract), plus a running summary every 1,000 orders.
void ArenaClient::log_latencies(std::size_t n) {
  std::size_t len = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const long long ns = tick_lat_ns_[i];
    if (lat_count_ == 0) lat_min_ns_ = lat_max_ns_ = ns;
    lat_min_ns_ = std::min(lat_min_ns_, ns);
    lat_max_ns_ = std::max(lat_max_ns_, ns);
    lat_sum_ns_ += ns;
    ++lat_count_;
    const int w = std::snprintf(log_buf_ + len, sizeof(log_buf_) - len, "LAT %.*s %.3f\n",
                                static_cast<int>(tick_symbol_->size()), tick_symbol_->c_str(),
                                ns / 1000.0);
    if (w > 0) len = std::min(sizeof(log_buf_) - 1, len + static_cast<std::size_t>(w));
  }
  std::fwrite(log_buf_, 1, len, stderr);
  if (lat_count_ % 1000 == 0)
    std::fprintf(stderr, "[latency] n=%lld avg=%.3f min=%.3f max=%.3f us\n", lat_count_,
                 lat_sum_ns_ / 1000.0 / lat_count_, lat_min_ns_ / 1000.0, lat_max_ns_ / 1000.0);
}

bool ArenaClient::book(const std::string& symbol, BookView& out) const {
  std::lock_guard<std::mutex> lock(book_mtx_);
  for (std::size_t i = 0; i < top_count_; ++i) {
    const Top& t = tops_[i];
    if (t.symbol != symbol) continue;
    out.symbol = t.symbol;
    out.best_bid = t.best_bid;
    out.best_ask = t.best_ask;
    out.mid = t.mid;
    out.spread = t.spread;
    out.ref = t.ref;
    out.microprice = t.microprice;
    out.obi = t.obi;
    out.bid_count = out.ask_count = 0;
    return true;
  }
  return false;
}

}  // namespace arena
