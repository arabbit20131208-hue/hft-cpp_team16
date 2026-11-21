// queue_strategy.hpp — Team 16 strategy: local book + queue-aware quoting
// under an order_quota (the Phase 2/3 logic, independent of transport).
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
//
// Orders leave through an OrderSink, so the same class runs inside the
// strategy process (sink = shared-memory ring to the gateway) or in tests.
#pragma once
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

#include "local_book.hpp"
#include "quote_policy.hpp"

namespace arena {

class OrderSink {
 public:
  virtual ~OrderSink() = default;
  virtual void place_limit(std::string_view symbol, bool buy, int quantity, double price) = 0;
  virtual void cancel(std::string_view order_id, std::string_view symbol) = 0;
  virtual bool quota_allows(bool cancel) = 0;
  virtual void request_stop() = 0;
};

// A book update, wherever it came from.
struct BookRef {
  std::string_view symbol;
  const Level* bids;
  int bid_count;
  const Level* asks;
  int ask_count;
  double mid, spread, microprice, obi;
};

class QueueStrategy {
 public:
  static constexpr int kMaxPosition = 5;   // per symbol, each direction
  static constexpr int kAckTimeout = 5;    // snapshots of that symbol
  static constexpr int kCancelGrace = 2;   // snapshots of that symbol
  static constexpr int kMaxErrors = 100;   // stop after this many non-fatal venue errors
  static constexpr std::size_t kMaxId = 128;
  static constexpr std::size_t kMaxSymbols = 64;
  static constexpr long long kRejectSamples = 3;

  QueueStrategy(double tick, OrderSink& sink, PolicyParams params = {})
      : params_(params), tick_(tick), sink_(sink) {
    if (!std::isfinite(tick_) || tick_ <= 0) throw std::runtime_error("invalid TICK_SIZE");
    for (auto& name : symbols_) name.reserve(kMaxId);
    for (auto& s : states_) {
      s.book = LocalBook(tick_);
      s.buy.id.reserve(kMaxId);
      s.sell.id.reserve(kMaxId);
    }
  }

  // ── hot path ─────────────────────────────────────────────────────────────
  void on_book(const BookRef& b, bool session_open) {
    ++book_stats_.snapshots;
    State* s = state(b.symbol);
    if (!s) {
      ++book_stats_.no_slot;
      return;
    }
    s->valid = s->book.update(b.bids, b.bid_count, b.asks, b.ask_count);
    if (!s->valid) {
      count_reject(s->book, b);
      return;
    }
    if (const int c = s->book.last_clipped()) {
      ++book_stats_.clipped_snapshots;
      book_stats_.clipped_levels += c;
    }
    check_against_venue(s->book, b);
    if (!session_open || !s->book.has_bid() || !s->book.has_ask()) return;
    s->signal.update(b.microprice, b.mid, s->book.ask() - s->book.bid(), b.obi);
    const double value = s->signal.value;
    step(*s, s->buy, true, value, b.symbol);
    step(*s, s->sell, false, -value, b.symbol);
  }

  // ── order lifecycle ───────────────────────────────────────────────────────
  void on_order_ack(std::string_view id, std::string_view symbol, std::string_view side,
                    int ahead, int level_qty) {
    State* s = state(symbol);
    Order* o = s ? side_order(*s, side) : nullptr;
    if (!o || o->status != Status::kPendingNew || id.empty() || id.size() > kMaxId) return;
    o->id.assign(id.data(), id.size());
    o->queue.update(ahead, level_qty);
    o->status = Status::kResting;
    o->age = 0;
  }

  void on_queue_update(std::string_view id, std::string_view symbol, std::string_view side,
                       int ahead, int level_qty) {
    State* s = state(symbol);
    Order* o = s ? side_order(*s, side) : nullptr;
    if (o && !o->id.empty() && o->id == id) o->queue.update(ahead, level_qty);
  }

  void on_fill(std::string_view side, std::string_view symbol, int quantity) {
    State* s = state(symbol);
    Order* o = s ? side_order(*s, side) : nullptr;
    if (!o || quantity <= 0) return;  // not our trade
    s->position += side == "buy" ? quantity : -quantity;
    ++queue_stats_.fills;
    o->reset();
  }

  // Order rejects and cancels of filled orders are recoverable (the pending
  // timeouts return the side to Idle); a flood of errors is not.
  void on_error(std::string_view code, std::string_view message) {
    if (++errors_ > kMaxErrors) {
      std::fprintf(stderr, "[strategy] stopping after %d errors (last %.*s: %.*s)\n", errors_,
                   static_cast<int>(code.size()), code.data(), static_cast<int>(message.size()),
                   message.data());
      sink_.request_stop();
    }
  }

  // After a restart: adopt the gateway's position (its open orders were cancelled).
  void set_position(std::string_view symbol, int position) {
    if (State* s = state(symbol)) s->position = position;
  }
  int position(std::string_view symbol) {
    State* s = state(symbol);
    return s ? s->position : 0;
  }

  void print_stats() const {
    const BookStats& b = book_stats_;
    const QueueStats& q = queue_stats_;
    std::fprintf(stderr,
                 "[book] snapshots=%lld rejected=%lld checked=%lld mid_mismatch=%lld "
                 "spread_mismatch=%lld\n",
                 b.snapshots, b.rejected, b.checked, b.mid_mismatch, b.spread_mismatch);
    std::fprintf(stderr, "[book] unchecked: one_sided=%lld no_mid=%lld no_slot=%lld\n",
                 b.one_sided, b.no_mid, b.no_slot);
    std::fprintf(stderr, "[book] clipped_snapshots=%lld clipped_levels=%lld\n",
                 b.clipped_snapshots, b.clipped_levels);
    std::fprintf(stderr, "[book] rejected by reason:");
    for (int i = 1; i < LocalBook::kRejectKinds; ++i)
      std::fprintf(stderr, " %s=%lld", LocalBook::reject_name(static_cast<LocalBook::Reject>(i)),
                   b.by_reason[i]);
    std::fprintf(stderr, "\n");
    std::fprintf(stderr,
                 "[queue] aware=%d orders=%lld fills=%lld held_near_front=%lld "
                 "cancel_behind_touch=%lld cancel_strong_adverse=%lld "
                 "cancel_adverse_far_back=%lld ack_timeouts=%lld errors=%d\n",
                 params_.queue_aware ? 1 : 0, q.orders, q.fills, q.held_near_front,
                 q.behind_touch, q.strong_adverse, q.adverse_far_back, q.ack_timeouts, errors_);
  }

 private:
  enum class Status { kIdle, kPendingNew, kResting, kPendingCancel };

  struct Order {
    Status status = Status::kIdle;
    std::string id;
    long long price_tick = 0;
    int age = 0;  // snapshots in the current status
    bool held_near_front = false;
    QueueState queue;
    void reset() {
      status = Status::kIdle;
      id.clear();
      age = 0;
      held_near_front = false;
      queue = {};
    }
  };

  struct State {
    LocalBook book;
    Signal signal;
    Order buy, sell;
    int position = 0;
    bool valid = false;
  };

  struct BookStats {
    long long snapshots = 0, rejected = 0, checked = 0, mid_mismatch = 0, spread_mismatch = 0;
    long long no_slot = 0, one_sided = 0, no_mid = 0;
    long long clipped_snapshots = 0, clipped_levels = 0;
    std::array<long long, LocalBook::kRejectKinds> by_reason{};
  };
  struct QueueStats {
    long long orders = 0, fills = 0, ack_timeouts = 0;
    long long behind_touch = 0, strong_adverse = 0, adverse_far_back = 0, held_near_front = 0;
  };

  // Linear over <= 64 names, last hit checked first. Names are reserved, so
  // registering a symbol never allocates.
  State* state(std::string_view symbol) {
    if (last_ < count_ && symbols_[last_] == symbol) return &states_[last_];
    for (int i = 0; i < count_; ++i)
      if (symbols_[i] == symbol) return &states_[last_ = i];
    if (count_ == static_cast<int>(states_.size()) || symbol.empty() || symbol.size() > kMaxId)
      return nullptr;
    symbols_[count_].assign(symbol.data(), symbol.size());
    last_ = count_;
    return &states_[count_++];
  }

  static Order* side_order(State& s, std::string_view side) {
    if (side == "buy") return &s.buy;
    if (side == "sell") return &s.sell;
    return nullptr;
  }

  // Advance one side's order by one snapshot. `edge` is the signal in this side's favour.
  void step(State& s, Order& o, bool buy, double edge, std::string_view symbol) {
    const long long touch = buy ? s.book.bid_tick() : s.book.ask_tick();
    switch (o.status) {
      case Status::kIdle: {
        const bool room = buy ? s.position < kMaxPosition : s.position > -kMaxPosition;
        if (edge <= params_.entry || !room) return;
        if (!sink_.quota_allows(false)) return;  // budget spent: stay Idle, try next tick
        o.status = Status::kPendingNew;
        o.price_tick = touch;
        o.age = 0;
        ++queue_stats_.orders;
        sink_.place_limit(symbol, buy, 1, s.book.tick_to_price(touch));
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
        const Decision d = resting_decision(behind, edge, o.queue, params_);
        if (d.action == Action::kHold) {
          if (d.reason == Reason::kHoldNearFront && !o.held_near_front) {
            o.held_near_front = true;
            ++queue_stats_.held_near_front;
          }
          return;
        }
        if (!sink_.quota_allows(true)) return;  // even the cancel reserve is spent
        count_cancel(d.reason);
        sink_.cancel(o.id, symbol);
        o.status = Status::kPendingCancel;
        o.age = 0;
        return;
      }
      case Status::kPendingCancel:
        if (++o.age >= kCancelGrace) o.reset();
        return;
    }
  }

  void count_cancel(Reason r) {
    switch (r) {
      case Reason::kBehindTouch: ++queue_stats_.behind_touch; break;
      case Reason::kStrongAdverse: ++queue_stats_.strong_adverse; break;
      case Reason::kAdverseFarBack: ++queue_stats_.adverse_far_back; break;
      default: break;
    }
  }

  void count_reject(const LocalBook& book, const BookRef& b) {
    ++book_stats_.rejected;
    const auto why = book.last_reject();
    long long& n = book_stats_.by_reason[static_cast<int>(why)];
    if (++n <= kRejectSamples)
      std::fprintf(stderr, "[book-reject] %.*s reason=%s bad_price=%.10g span_ticks=%lld levels=%d/%d\n",
                   static_cast<int>(b.symbol.size()), b.symbol.data(), LocalBook::reject_name(why),
                   book.bad_price(), book.bad_span(), b.bid_count, b.ask_count);
  }

  // Compare the local touch with the venue's own mid_price / spread.
  void check_against_venue(const LocalBook& book, const BookRef& b) {
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
  }

  PolicyParams params_;
  double tick_;
  OrderSink& sink_;
  std::array<std::string, kMaxSymbols> symbols_;
  std::array<State, kMaxSymbols> states_;
  int count_ = 0, last_ = 0, errors_ = 0;
  BookStats book_stats_;
  QueueStats queue_stats_;
};

}  // namespace arena
