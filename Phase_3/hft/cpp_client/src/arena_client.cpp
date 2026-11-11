// arena_client.cpp — Phase 3 threaded pipeline (see arena_client.hpp).
// Outbound JSON matches shared/messages.py (PlaceOrder / CancelOrder / Handshake).
#include "arena_client.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace arena {
namespace {

std::string env_or(const char* key, const std::string& def) {
  const char* v = std::getenv(key);
  if (v == nullptr || *v == '\0') return def;
  return std::string(v);
}

template <class T>
T env_num(const char* key, T def) {
  try {
    const std::string v = env_or(key, "");
    if (v.empty()) return def;
    return static_cast<T>(std::stod(v));
  } catch (...) {
    return def;
  }
}

constexpr std::uint64_t kNoSlot = ~0ULL;

}  // namespace

ClientConfig ClientConfig::from_env() {
  ClientConfig c;
  c.team_id = env_or("TEAM_ID", "hft_alpha");
  c.token = env_or("ARENA_TOKEN", "");
  c.tick_size = env_num("TICK_SIZE", 0.01);
  const std::string url = env_or("EXCHANGE_URL", "");
  c.url = !url.empty() ? url
                       : "ws://" + env_or("EXCHANGE_HOST", "localhost") + ":" +
                             env_or("EXCHANGE_PORT", "8765");
  c.level = env_num("ARENA_LEVEL", 1);
  c.send_thread = env_or("SEND_THREAD", "1") != "0";
  c.order_quota = env_num("ORDER_QUOTA", 6);
  c.quota_window_ms = env_num("ORDER_QUOTA_MS", 1000LL);
  std::stringstream cores(env_or("PIN_CORES", ""));
  std::string item;
  for (std::size_t i = 0; i < c.cores.size() && std::getline(cores, item, ','); ++i) {
    try {
      c.cores[i] = std::stoi(item);
    } catch (...) {
      c.cores[i] = -1;
    }
  }
  return c;
}

ArenaClient::ArenaClient(ClientConfig cfg)
    : cfg_(std::move(cfg)),
      in_(new Event[kInSlots]),
      out_(new OutSlot[kOutSlots]),
      quota_(cfg_.order_quota, cfg_.quota_window_ms * 1000000LL) {
  if (cfg_.team_id.size() > 128) throw std::length_error("team_id exceeds 128 bytes");
  // Every slot starts free. Filled before any worker starts, so thread
  // creation orders these pushes before the workers' first pops.
  for (std::uint64_t i = 0; i < kInSlots; ++i) free_in_.push(i);
  for (std::uint64_t i = 0; i < kOutSlots; ++i) free_out_.push(i);
  line_.reserve(kMaxTickMessages * 512 + 4);
  ws_.setUrl(cfg_.url);
  ws_.setPingInterval(15);
  ws_.setMaxWaitBetweenReconnectionRetries(3000);
  connect_handlers();
}

ArenaClient::~ArenaClient() {
  running_.store(false, std::memory_order_release);
  ws_.stop();
  join_workers();
}

void ArenaClient::stop() { running_.store(false, std::memory_order_release); }

// ── lifecycle ───────────────────────────────────────────────────────────────

void ArenaClient::start_workers() {
  strategy_thread_ = std::thread([this] { strategy_loop(); });
  if (cfg_.send_thread) send_thread_ = std::thread([this] { send_loop(); });
}

void ArenaClient::join_workers() {
  if (strategy_thread_.joinable()) strategy_thread_.join();
  if (send_thread_.joinable()) send_thread_.join();
}

void ArenaClient::run() {
  running_.store(true, std::memory_order_release);
  start_workers();
  ws_.start();
  while (running_.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ws_.stop();  // joins the socket (rx) thread: no more ingest() after this
  join_workers();
  print_pipeline_stats();
  on_stop();
}

void ArenaClient::run_replay(std::istream& in, std::ostream& out) {
  replay_mode_ = true;
  replay_out_ = &out;
  session_open_.store(true, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  start_workers();
  std::string line;
  line.reserve(1 << 20);
  while (std::getline(in, line)) ingest(line, true);  // this thread is the rx stage
  std::uint64_t slot;
  if (acquire_in_slot(slot, line, true)) {  // EOF: a stop event drains the pipeline
    in_[slot].type = EventType::kStop;
    in_[slot].recv = in_[slot].pushed = clock::now();
    while (!ticks_.push(slot)) cpu_relax();
  }
  join_workers();
  running_.store(false, std::memory_order_release);
  print_pipeline_stats();
  on_stop();
}

// ── rx stage ────────────────────────────────────────────────────────────────

void ArenaClient::connect_handlers() {
  ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        std::cerr << "[arena] connected to " << cfg_.url << " as " << cfg_.team_id
                  << " (trader)\n";
        session_open_.store(false, std::memory_order_release);
        send_handshake();
        break;
      case ix::WebSocketMessageType::Message:
        ingest(msg->str, false);
        break;
      case ix::WebSocketMessageType::Close:
        std::cerr << "[arena] connection closed (" << msg->closeInfo.code << ": "
                  << msg->closeInfo.reason << ") — reconnecting\n";
        session_open_.store(false, std::memory_order_release);
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
  ws_.sendText(hs.dump());  // IXWebSocket::sendText is thread-safe
}

// Take a free Event slot. If the strategy thread has fallen so far behind that
// all kInSlots slots are in flight, a live book snapshot is dropped (counted):
// a later snapshot replaces it anyway. Every other message waits, so acks and
// fills are never lost, and replay never drops (one output line per input).
bool ArenaClient::acquire_in_slot(std::uint64_t& slot, const std::string& raw, bool replay) {
  if (spare_in_ != kNoSlot) {
    slot = spare_in_;
    spare_in_ = kNoSlot;
    return true;
  }
  Backoff backoff;
  while (!free_in_.pop(slot)) {
    if (!replay && raw.find("\"book_snapshot\"") != std::string::npos) {
      dropped_books_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (!running_.load(std::memory_order_acquire) && !replay) return false;
    backoff.idle();
  }
  return true;
}

void ArenaClient::ingest(const std::string& raw, bool replay) {
  const time_point recv = clock::now();
  if (!rx_pinned_) {
    pin_current_thread(cfg_.cores[0]);
    rx_pinned_ = true;
  }
  std::uint64_t slot;
  if (!acquire_in_slot(slot, raw, replay)) return;
  Event& e = in_[slot];
  e.recv = recv;
  if (rx_codec_.decode(raw, e.book)) {
    e.type = EventType::kBook;
  } else {
    decode_other(raw, e);
  }
  if (e.type == EventType::kIgnored && !replay) {
    spare_in_ = slot;  // nothing for the strategy: keep the slot for the next frame
    return;
  }
  e.pushed = clock::now();
  const long long backlog = ++produced_ - consumed_.load(std::memory_order_relaxed);
  max_backlog_ = std::max(max_backlog_, backlog);
  while (!ticks_.push(slot)) cpu_relax();  // cannot stay full: it holds every slot
}

// Non-snapshot frames: rarer, parsed with the DOM on the rx thread so the
// strategy thread only sees plain fields.
void ArenaClient::decode_other(const std::string& raw, Event& e) {
  e.type = EventType::kIgnored;
  if (raw.empty()) return;
  json m;
  try {
    m = json::parse(raw);
  } catch (const std::exception& ex) {
    std::cerr << "[arena] bad JSON frame: " << ex.what() << "\n";
    return;
  }
  const std::string type = m.value("type", "");
  if (type == "book_snapshot") {
    const long long n = malformed_books_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3) std::cerr << "[arena] malformed book_snapshot dropped: " << raw.substr(0, 300) << "\n";
  } else if (type == "trade_execution") {
    e.type = EventType::kFill;
    const std::string buyer = m.value("buyer_id", "");
    const std::string seller = m.value("seller_id", "");
    e.side = buyer == cfg_.team_id ? "buy" : seller == cfg_.team_id ? "sell" : "";
    e.maker = m.value("maker_id", "") == cfg_.team_id;
    e.symbol = m.value("symbol", "");
    e.quantity = m.value("quantity", 0);
    e.price = m.value("price", 0.0);
  } else if (type == "portfolio_update") {
    e.type = EventType::kPortfolio;
    e.cash = m.value("cash", 0.0);
    e.net_worth = m.value("net_worth", 0.0);
    e.realized = m.value("realized_pnl", 0.0);
    e.unrealized = m.value("unrealized_pnl", 0.0);
  } else if (type == "session_event") {
    e.type = EventType::kSession;
    e.code = m.value("event", "");
    e.text = m.value("message", "");
    if (e.code == "SESSION_OPEN" || e.code == "SESSION_PREOPEN")
      session_open_.store(true, std::memory_order_release);
    else if (e.code == "SESSION_CLOSED")
      session_open_.store(false, std::memory_order_release);
  } else if (type == "order_ack" || type == "queue_update") {
    e.type = type == "order_ack" ? EventType::kAck : EventType::kQueue;
    e.order_id = m.value("order_id", "");
    e.symbol = m.value("symbol", "");
    e.side = m.value("side", "");
    e.price = m.value("price", 0.0);
    e.quantity = m.value("quantity", 0);
    e.queue_ahead = m.value("queue_ahead", -1);
    e.level_qty = m.value("level_qty", 0);
  } else if (type == "error") {
    e.type = EventType::kError;
    e.code = m.value("code", "");
    e.text = m.value("message", "");
    std::cerr << "[arena] server error " << e.code << ": " << e.text << "\n";
  }
}

// ── strategy stage ──────────────────────────────────────────────────────────

void ArenaClient::strategy_loop() {
  pin_current_thread(cfg_.cores[1]);
  Backoff backoff;
  std::uint64_t i;
  for (;;) {
    if (!ticks_.pop(i)) {
      if (!running_.load(std::memory_order_acquire)) break;
      backoff.idle();
      continue;
    }
    backoff.reset();
    Event& e = in_[i];
    std::uint64_t o = 0;
    if (cfg_.send_thread) {
      Backoff wait;  // only waits if the send thread is kOutSlots ticks behind
      while (!free_out_.pop(o)) wait.idle();
    }
    OutSlot& out = out_[o];
    out.popped = clock::now();
    handle(e, out);
    out.decided = clock::now();
    const bool stop = e.type == EventType::kStop;
    while (!free_in_.push(i)) cpu_relax();
    consumed_.fetch_add(1, std::memory_order_relaxed);
    if (cfg_.send_thread) {
      while (!sends_.push(o)) cpu_relax();
    } else {
      deliver(out);
    }
    if (stop) break;
  }
  strategy_done_.store(true, std::memory_order_release);
}

void ArenaClient::handle(Event& e, OutSlot& out) {
  out.count = 0;
  out.book = e.type == EventType::kBook;
  out.stop = e.type == EventType::kStop;
  out.recv = e.recv;
  out.pushed = e.pushed;
  event_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(e.recv.time_since_epoch()).count();
  messages_.reset();
  switch (e.type) {
    case EventType::kBook:
      out.symbol = e.book.symbol;
      on_book_snapshot(e.book);
      break;
    case EventType::kAck:
      on_order_ack(e.order_id, e.symbol, e.side, e.price, e.quantity, e.queue_ahead, e.level_qty);
      break;
    case EventType::kQueue:
      on_queue_update(e.order_id, e.symbol, e.side, e.price, e.queue_ahead, e.level_qty);
      break;
    case EventType::kFill:
      on_fill(e.side, e.symbol, e.quantity, e.price, e.maker);
      break;
    case EventType::kPortfolio:
      on_portfolio(e.cash, e.net_worth, e.realized, e.unrealized);
      break;
    case EventType::kSession:
      on_session(e.code, e.text);
      break;
    case EventType::kError:
      on_error(e.code, e.text);
      break;
    case EventType::kIgnored:
    case EventType::kStop:
      break;
  }
  for (std::size_t k = 0; k < messages_.offset; ++k)
    tx_codec_.encode(messages_.slots[k], cfg_.team_id, out.msgs[k]);
  out.count = messages_.offset;
}

void ArenaClient::queue(const Message& m) {
  Message* slot = messages_.alloc();
  if (slot) *slot = m;  // more than kMaxTickMessages in one event: dropped, never allocated
}

void ArenaClient::place_limit(const std::string& symbol, const std::string& side, int quantity,
                              double price) {
  PlaceOrder o;
  if (!o.symbol.set(symbol)) return;
  o.buy = side == "buy";
  o.quantity = quantity;
  o.price = price;
  queue(o);
}

void ArenaClient::place_market(const std::string& symbol, const std::string& side,
                               int quantity) {
  PlaceOrder o;
  if (!o.symbol.set(symbol)) return;
  o.buy = side == "buy";
  o.quantity = quantity;
  o.market = true;
  queue(o);
}

void ArenaClient::cancel(const std::string& order_id, const std::string& symbol) {
  CancelOrder o;
  if (!o.symbol.set(symbol) || !o.order_id.set(order_id)) return;
  queue(o);
}

// ── send stage ──────────────────────────────────────────────────────────────

void ArenaClient::send_loop() {
  pin_current_thread(cfg_.cores[2]);
  Backoff backoff;
  std::uint64_t o;
  for (;;) {
    if (sends_.pop(o)) {
      backoff.reset();
      const bool stop = out_[o].stop;
      deliver(out_[o]);
      while (!free_out_.push(o)) cpu_relax();
      if (stop) break;
      continue;
    }
    if (strategy_done_.load(std::memory_order_acquire) && sends_.empty()) break;
    backoff.idle();
  }
}

void ArenaClient::deliver(OutSlot& out) {
  const time_point start = clock::now();
  if (replay_mode_) {
    if (!out.stop) {
      line_.assign(1, '[');
      for (std::size_t k = 0; k < out.count; ++k) {
        if (k) line_ += ',';
        line_ += out.msgs[k];
      }
      line_ += "]\n";
      replay_out_->write(line_.data(), static_cast<std::streamsize>(line_.size()));
      replay_out_->flush();
    }
  } else {
    for (std::size_t k = 0; k < out.count; ++k) ws_.sendText(out.msgs[k]);
  }
  log_slot(out, start, clock::now());
}

// LAT (course contract): frame arrival -> the tick's orders handed to the
// socket, one line per order. PROF (--profile): per-stage breakdown of every
// snapshot. Both are written here, after the send.
void ArenaClient::log_slot(const OutSlot& out, time_point send_start, time_point send_end) {
  if (!out.book) return;
  const long long total = ns_between(out.recv, send_end);
  if (cfg_.profile)
    std::fprintf(stderr, "PROF %lld %lld %lld %lld %lld %lld\n", ns_between(out.recv, out.pushed),
                 ns_between(out.pushed, out.popped), ns_between(out.popped, out.decided),
                 ns_between(out.decided, send_start), ns_between(send_start, send_end), total);
  if (out.count == 0) return;
  std::size_t len = 0;
  for (std::size_t k = 0; k < out.count; ++k) {
    const int w = std::snprintf(log_buf_ + len, sizeof(log_buf_) - len, "LAT %s %.3f\n",
                                out.symbol.c_str(), total / 1000.0);
    if (w > 0) len = std::min(sizeof(log_buf_) - 1, len + static_cast<std::size_t>(w));
    ++lat_count_;
    lat_sum_ns_ += total;
    lat_max_ns_ = std::max(lat_max_ns_, total);
  }
  std::fwrite(log_buf_, 1, len, stderr);
  if (lat_count_ % 1000 < static_cast<long long>(out.count))
    std::fprintf(stderr, "[latency] n=%lld avg=%.3f max=%.3f us\n", lat_count_,
                 lat_sum_ns_ / 1000.0 / lat_count_, lat_max_ns_ / 1000.0);
}

// Main thread, after every worker has been joined.
void ArenaClient::print_pipeline_stats() const {
  std::fprintf(stderr,
               "[pipeline] events=%lld max_backlog=%lld dropped_books=%lld send_thread=%d "
               "cores=%d,%d,%d\n",
               produced_, max_backlog_, dropped_books_.load(), cfg_.send_thread ? 1 : 0,
               cfg_.cores[0], cfg_.cores[1], cfg_.cores[2]);
  std::fprintf(stderr, "[quota] order_quota=%d window_ms=%lld max_used_in_window=%d skipped=%lld\n",
               quota_.quota(), quota_.window_ns() / 1000000, quota_.max_used_in_window(),
               quota_.skipped());
}

}  // namespace arena
