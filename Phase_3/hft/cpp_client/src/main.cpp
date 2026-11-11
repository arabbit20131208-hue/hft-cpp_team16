// main.cpp — Team 16 Phase 3 strategy: local book + queue-aware quoting under an
// order_quota. Runs on the pipeline's strategy thread (see arena_client.hpp).
//
// Per symbol: a flat LocalBook rebuilt from each snapshot, an EWMA signal, and
// at most one order per side (size 1). Each order walks a small state machine:
//
//   Idle --signal past entry--> PendingNew --order_ack--> Resting
//   Resting --resting_decision() says cancel--> PendingCancel --grace--> Idle
//   any state --fill--> Idle
//
// The protocol has no cancel confirmation, so PendingCancel returns to Idle
// after kCancelGrace snapshots without a fill. PendingNew without an ack for
// kAckTimeout snapshots is treated as rejected.
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "hft_bot.hpp"
#include "quote_policy.hpp"

namespace {

constexpr int kMaxPosition = 5;   // per symbol, each direction
constexpr int kAckTimeout = 5;    // snapshots of that symbol
constexpr int kCancelGrace = 2;   // snapshots of that symbol
constexpr int kMaxErrors = 100;   // stop after this many non-fatal venue errors
constexpr std::size_t kMaxId = 128;
constexpr std::size_t kMaxSymbols = 64;
constexpr long long kRejectSamples = 3;  // rejection examples printed per reason

class QueueBot : public arena::HFTBot {
 public:
  explicit QueueBot(arena::ClientConfig cfg) : HFTBot(cfg), tick_(cfg.tick_size) {
    if (!std::isfinite(tick_) || tick_ <= 0) throw std::runtime_error("invalid TICK_SIZE");
    const char* aware = std::getenv("QUEUE_AWARE");
    params_.queue_aware = !(aware && std::string(aware) == "0");
    for (auto& name : symbols_) name.reserve(kMaxId);
    for (auto& s : states_) {
      s.book = arena::LocalBook(tick_);
      s.buy.id.reserve(kMaxId);
      s.sell.id.reserve(kMaxId);
    }
  }

 private:
  enum class Status { kIdle, kPendingNew, kResting, kPendingCancel };

  struct Order {
    Status status = Status::kIdle;
    std::string id;
    long long price_tick = 0;
    int age = 0;  // snapshots in the current status
    bool held_near_front = false;
    arena::QueueState queue;
    void reset() {
      status = Status::kIdle;
      id.clear();
      age = 0;
      held_near_front = false;
      queue = {};
    }
  };

  struct State {
    arena::LocalBook book;
    arena::Signal signal;
    Order buy, sell;
    int position = 0;
    bool valid = false;
  };

  struct BookStats {  // local book vs venue fields
    long long snapshots = 0, rejected = 0, checked = 0, mid_mismatch = 0, spread_mismatch = 0;
    long long no_slot = 0, one_sided = 0, no_mid = 0;  // accepted but not checked
    long long clipped_snapshots = 0, clipped_levels = 0;  // deep levels outside the band
    std::array<long long, arena::LocalBook::kRejectKinds> by_reason{};
  };
  struct QueueStats {  // what the policy did
    long long orders = 0, fills = 0, ack_timeouts = 0;
    long long behind_touch = 0, strong_adverse = 0, adverse_far_back = 0, held_near_front = 0;
  };

  // ── symbol lookup (linear over <= 64 names, last hit checked first) ────
  State* state(const std::string& symbol) {
    if (last_ < count_ && symbols_[last_] == symbol) return &states_[last_];
    for (int i = 0; i < count_; ++i)
      if (symbols_[i] == symbol) return &states_[last_ = i];
    if (count_ == static_cast<int>(states_.size()) || symbol.size() > kMaxId) return nullptr;
    symbols_[count_] = symbol;
    last_ = count_;
    return &states_[count_++];
  }

  // ── hot path ─────────────────────────────────────────────────────────────
  void on_depth(const arena::BookView& b) override {
    current_ = state(b.symbol);
    ++book_stats_.snapshots;
    if (!current_) {
      ++book_stats_.no_slot;
      return;
    }
    State& s = *current_;
    s.valid = s.book.update(b);
    if (!s.valid) {
      count_reject(s.book, b);
      return;
    }
    if (const int c = s.book.last_clipped()) {
      ++book_stats_.clipped_snapshots;
      book_stats_.clipped_levels += c;
    }
    check_against_venue(s.book, b);
  }

  // Rejections are rare by design; this path is cold. The first few of each
  // kind are printed so a live run shows what the venue actually sent.
  void count_reject(const arena::LocalBook& book, const arena::BookView& b) {
    ++book_stats_.rejected;
    const auto why = book.last_reject();
    long long& n = book_stats_.by_reason[static_cast<int>(why)];
    if (++n <= kRejectSamples)
      std::fprintf(stderr,
                   "[book-reject] %s reason=%s bad_price=%.10g span_ticks=%lld levels=%d/%d "
                   "venue_bid=%.10g venue_ask=%.10g\n",
                   b.symbol.c_str(), arena::LocalBook::reject_name(why), book.bad_price(),
                   book.bad_span(), b.bid_count, b.ask_count, b.best_bid, b.best_ask);
  }

  void on_book(const std::string& symbol, double, double, double mid, double microprice,
               double obi) override {
    if (!current_ || !current_->valid) return;
    State& s = *current_;
    if (!s.book.has_bid() || !s.book.has_ask()) return;
    s.signal.update(microprice, mid, s.book.ask() - s.book.bid(), obi);
    const double value = s.signal.value;
    step(s, s.buy, true, value, symbol);
    step(s, s.sell, false, -value, symbol);
  }

  // Advance one side's order by one snapshot. `edge` is the signal in this
  // side's favour.
  void step(State& s, Order& o, bool buy, double edge, const std::string& symbol) {
    const long long touch = buy ? s.book.bid_tick() : s.book.ask_tick();
    switch (o.status) {
      case Status::kIdle: {
        const bool room = buy ? s.position < kMaxPosition : s.position > -kMaxPosition;
        if (edge <= params_.entry || !room) return;
        if (!quota_allows(false)) return;  // budget spent: stay Idle, try next tick
        o.status = Status::kPendingNew;
        o.price_tick = touch;
        o.age = 0;
        ++queue_stats_.orders;
        const double price = s.book.tick_to_price(touch);
        if (buy) buy_limit(symbol, 1, price);
        else sell_limit(symbol, 1, price);
        return;
      }
      case Status::kPendingNew:
        if (++o.age >= kAckTimeout) {
          ++queue_stats_.ack_timeouts;
          o.reset();
        }
        return;
      case Status::kResting: {
        const bool behind = buy ? touch > o.price_tick : touch < o.price_tick;
        const arena::Decision d = arena::resting_decision(behind, edge, o.queue, params_);
        if (d.action == arena::Action::kHold) {
          if (d.reason == arena::Reason::kHoldNearFront && !o.held_near_front) {
            o.held_near_front = true;
            ++queue_stats_.held_near_front;
          }
          return;
        }
        if (!quota_allows(true)) return;  // even the cancel reserve is spent: retry next tick
        count_cancel(d.reason);
        cancel_order(o.id, symbol);
        o.status = Status::kPendingCancel;
        o.age = 0;
        return;
      }
      case Status::kPendingCancel:
        if (++o.age >= kCancelGrace) o.reset();
        return;
    }
  }

  void count_cancel(arena::Reason r) {
    switch (r) {
      case arena::Reason::kBehindTouch: ++queue_stats_.behind_touch; break;
      case arena::Reason::kStrongAdverse: ++queue_stats_.strong_adverse; break;
      case arena::Reason::kAdverseFarBack: ++queue_stats_.adverse_far_back; break;
      default: break;
    }
  }

  // Compare the local book's touch with the venue's own mid_price/spread.
  void check_against_venue(const arena::LocalBook& book, const arena::BookView& b) {
    if (!book.has_bid() || !book.has_ask()) {
      ++book_stats_.one_sided;
      return;
    }
    if (!(b.mid > 0)) {
      ++book_stats_.no_mid;
      return;
    }
    ++book_stats_.checked;
    const double tol = 0.25 * tick_;
    const bool mid_bad = std::abs(0.5 * (book.bid() + book.ask()) - b.mid) > tol;
    const bool spread_bad = b.spread > 0 && std::abs(book.ask() - book.bid() - b.spread) > tol;
    book_stats_.mid_mismatch += mid_bad;
    book_stats_.spread_mismatch += spread_bad;
    if ((mid_bad || spread_bad) &&
        book_stats_.mid_mismatch + book_stats_.spread_mismatch <= kRejectSamples)
      std::fprintf(stderr,
                   "[book-mismatch] %s local_bid=%.10g local_ask=%.10g venue_mid=%.10g "
                   "venue_spread=%.10g\n",
                   b.symbol.c_str(), book.bid(), book.ask(), b.mid, b.spread);
  }

  // ── order lifecycle (off the book path) ─────────────────────────────────
  Order* side_order(State& s, const std::string& side) {
    if (side == "buy") return &s.buy;
    if (side == "sell") return &s.sell;
    return nullptr;
  }

  void on_order_ack(const std::string& id, const std::string& symbol, const std::string& side,
                    double, int, int ahead, int level_qty) override {
    State* s = state(symbol);
    Order* o = s ? side_order(*s, side) : nullptr;
    if (!o || o->status != Status::kPendingNew || id.empty() || id.size() > kMaxId) return;
    o->id = id;
    o->queue.update(ahead, level_qty);
    o->status = Status::kResting;
    o->age = 0;
  }

  void on_queue_update(const std::string& id, const std::string& symbol,
                       const std::string& side, double, int ahead, int level_qty) override {
    State* s = state(symbol);
    Order* o = s ? side_order(*s, side) : nullptr;
    if (o && !o->id.empty() && o->id == id) o->queue.update(ahead, level_qty);
  }

  void on_fill(const std::string& side, const std::string& symbol, int quantity, double,
               bool) override {
    State* s = state(symbol);
    Order* o = s ? side_order(*s, side) : nullptr;
    if (!o || quantity <= 0) return;  // not our trade
    s->position += side == "buy" ? quantity : -quantity;
    ++queue_stats_.fills;
    o->reset();
  }

  void on_error(const std::string& code, const std::string& message) override {
    // Auth problems are configuration errors: stop. Anything else (e.g. an
    // order rejected or a cancel for an order that already filled) is
    // recoverable: the pending-order timeouts return that side to Idle.
    if (code.rfind("AUTH", 0) == 0 || ++errors_ > kMaxErrors) {
      std::fprintf(stderr, "[bot] stopping after error %s: %s\n", code.c_str(), message.c_str());
      stop();
    }
  }

  void on_stop() override {
    const BookStats& b = book_stats_;
    const QueueStats& q = queue_stats_;
    std::fprintf(stderr,
                 "[book] snapshots=%lld rejected=%lld checked=%lld mid_mismatch=%lld "
                 "spread_mismatch=%lld\n",
                 b.snapshots, b.rejected, b.checked, b.mid_mismatch, b.spread_mismatch);
    std::fprintf(stderr,
                 "[book] unchecked: one_sided=%lld no_mid=%lld no_slot=%lld "
                 "decoder_dropped=%lld\n",
                 b.one_sided, b.no_mid, b.no_slot, malformed_books());
    std::fprintf(stderr, "[book] clipped_snapshots=%lld clipped_levels=%lld\n",
                 b.clipped_snapshots, b.clipped_levels);
    std::fprintf(stderr, "[book] rejected by reason:");
    for (int i = 1; i < arena::LocalBook::kRejectKinds; ++i)
      std::fprintf(stderr, " %s=%lld",
                   arena::LocalBook::reject_name(static_cast<arena::LocalBook::Reject>(i)),
                   b.by_reason[i]);
    std::fprintf(stderr, "\n");
    std::fprintf(stderr,
                 "[queue] aware=%d orders=%lld fills=%lld held_near_front=%lld "
                 "cancel_behind_touch=%lld cancel_strong_adverse=%lld "
                 "cancel_adverse_far_back=%lld ack_timeouts=%lld errors=%d\n",
                 params_.queue_aware ? 1 : 0, q.orders, q.fills, q.held_near_front,
                 q.behind_touch, q.strong_adverse, q.adverse_far_back, q.ack_timeouts, errors_);
  }

  arena::PolicyParams params_;
  double tick_;
  std::array<std::string, kMaxSymbols> symbols_;
  std::array<State, kMaxSymbols> states_;
  int count_ = 0, last_ = 0, errors_ = 0;
  State* current_ = nullptr;
  BookStats book_stats_;
  QueueStats queue_stats_;
};

std::atomic<arena::HFTBot*> bot_ptr{nullptr};
void handle_signal(int) {
  if (auto* b = bot_ptr.load()) b->stop();
}

}  // namespace

int main(int argc, char** argv) {
  auto cfg = arena::ClientConfig::from_env();
  bool replay = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--replay") replay = true;
    else if (arg == "--profile") cfg.profile = true;
  }
  // ~2.5 MB of fixed per-symbol books: allocated once, before any callback.
  auto bot = std::make_unique<QueueBot>(cfg);
  if (replay) {
    bot->run_replay(std::cin, std::cout);
    return 0;
  }
  bot_ptr.store(bot.get());
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  bot->run();
  bot_ptr.store(nullptr);
}
