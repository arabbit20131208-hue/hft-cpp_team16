// local_book.hpp — Phase 2 local order book, signal, and queue state.
//
// LocalBook is a flat, price-indexed book rebuilt from every BookSnapshot.
// Prices become integer ticks relative to a per-snapshot base (the lowest
// price in the snapshot), so a level lives at bids_[tick - base_]. The touch
// is cached during the update, which makes bid()/ask() O(1).
//
// Cache behaviour of update():
//   * the snapshot's levels are read once, sequentially;
//   * the previous snapshot's slots are cleared through a short index list
//     (touched_*), not by sweeping the whole 16 KB array;
//   * a 10-level book with one-tick spacing writes ~10 consecutive ints per
//     side, i.e. one or two cache lines;
//   * there is no allocation and no pointer chasing.
//
// The band covers kCapacity ticks ($40.96 at a 0.01 tick). Deeper levels than
// that are clipped, never the touch (see update()).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace arena {

constexpr int kMaxDepth = 256;  // levels per side accepted from one snapshot

struct Level {
  double price = 0;
  int quantity = 0;
};

// Raw depth decoded from a book_snapshot (filled by FastCodec).
struct BookDepth {
  std::array<Level, kMaxDepth> bids{}, asks{};
  int bid_count = 0, ask_count = 0;
};

class LocalBook {
 public:
  static constexpr int kCapacity = 4096;  // ticks one snapshot may span

  explicit LocalBook(double tick = 0.01) : tick_(tick), inv_tick_(1.0 / tick) {
    // 1/0.01 is exactly 100; snap near-integers so tick_to_price(9999) == 99.99.
    if (std::abs(inv_tick_ - std::round(inv_tick_)) < 1e-9) inv_tick_ = std::round(inv_tick_);
  }

  // Why the last update() returned false (kNone after a success).
  enum class Reject { kNone, kTooDeep, kBadSize, kOffGrid, kSpan, kDuplicate, kLocked, kCrossed };
  static constexpr int kRejectKinds = 8;
  static const char* reject_name(Reject r) {
    static const char* names[kRejectKinds] = {"none",  "too_deep",  "bad_size", "off_grid",
                                              "span",  "duplicate", "locked",   "crossed"};
    return names[static_cast<int>(r)];
  }

  // Replace the visible book with snapshot `s`.
  //
  // The band is kCapacity ticks wide. If the snapshot fits, the band starts at
  // its lowest price. If deep levels make it wider, the band is re-anchored so
  // the touch sits in its middle, and levels more than kCapacity/2 ticks from
  // the touch are clipped (counted in last_clipped(), quantity() returns 0 for
  // them). The touch itself is never clipped.
  //
  // On invalid input the book is left empty, false is returned, and
  // last_reject()/bad_price() say why. kSpan now only means the spread itself
  // is wider than the band.
  bool update(const BookDepth& s) {
    clear();
    reject_ = Reject::kNone;
    clipped_ = 0;
    if (s.bid_count < 0 || s.bid_count > kMaxDepth || s.ask_count < 0 ||
        s.ask_count > kMaxDepth)
      return fail(Reject::kTooDeep);

    // Pass 1: convert every price to ticks once; find the band and the touch.
    long long low = std::numeric_limits<long long>::max();
    long long high = std::numeric_limits<long long>::min();
    long long top_bid = std::numeric_limits<long long>::min();
    long long top_ask = std::numeric_limits<long long>::max();
    if (!to_ticks(s.bids, s.bid_count, bid_ticks_, low, high) ||
        !to_ticks(s.asks, s.ask_count, ask_ticks_, low, high))
      return false;  // reject_ set by to_ticks
    if (s.bid_count + s.ask_count == 0) return true;  // empty book is valid
    for (int i = 0; i < s.bid_count; ++i) top_bid = std::max(top_bid, bid_ticks_[i]);
    for (int i = 0; i < s.ask_count; ++i) top_ask = std::min(top_ask, ask_ticks_[i]);

    if (high - low < kCapacity) {
      base_ = low;
    } else {
      const bool two_sided = s.bid_count > 0 && s.ask_count > 0;
      if (two_sided && top_ask - top_bid >= kCapacity - 2) {
        span_ = top_ask - top_bid;
        return fail(Reject::kSpan);
      }
      const long long anchor = !two_sided         ? (s.bid_count ? top_bid : top_ask)
                               : top_ask > top_bid ? top_bid + (top_ask - top_bid) / 2
                                                   : top_bid;  // crossed: rejected below
      base_ = anchor - kCapacity / 2;
    }

    // Pass 2: write sizes inside the band and track the touch.
    for (int i = 0; i < s.bid_count; ++i) {
      const long long off = bid_ticks_[i] - base_;
      if (off < 0 || off >= kCapacity) {
        ++clipped_;
        continue;
      }
      const int idx = static_cast<int>(off);
      if (bids_[idx] != 0) return fail(Reject::kDuplicate, s.bids[i].price);
      bids_[idx] = s.bids[i].quantity;
      touched_bids_[nb_++] = idx;
      if (idx > best_bid_) best_bid_ = idx;
    }
    for (int i = 0; i < s.ask_count; ++i) {
      const long long off = ask_ticks_[i] - base_;
      if (off < 0 || off >= kCapacity) {
        ++clipped_;
        continue;
      }
      const int idx = static_cast<int>(off);
      if (asks_[idx] != 0) return fail(Reject::kDuplicate, s.asks[i].price);
      asks_[idx] = s.asks[i].quantity;
      touched_asks_[na_++] = idx;
      if (best_ask_ < 0 || idx < best_ask_) best_ask_ = idx;
    }
    if (best_bid_ >= 0 && best_ask_ >= 0 && best_bid_ >= best_ask_) {
      const double bid = tick_to_price(base_ + best_bid_);
      return fail(best_bid_ == best_ask_ ? Reject::kLocked : Reject::kCrossed, bid);
    }
    return true;
  }

  // Levels dropped by the last update because they were outside the band.
  int last_clipped() const { return clipped_; }
  Reject last_reject() const { return reject_; }
  double bad_price() const { return bad_price_; }  // offending price, if any
  long long bad_span() const { return span_; }     // ticks, for kSpan

  // ── O(1) reads ───────────────────────────────────────────────────────────
  bool has_bid() const { return best_bid_ >= 0; }
  bool has_ask() const { return best_ask_ >= 0; }
  long long bid_tick() const { return has_bid() ? base_ + best_bid_ : -1; }
  long long ask_tick() const { return has_ask() ? base_ + best_ask_ : -1; }
  double bid() const { return has_bid() ? tick_to_price(bid_tick()) : 0; }
  double ask() const { return has_ask() ? tick_to_price(ask_tick()) : 0; }
  int bid_qty() const { return has_bid() ? bids_[best_bid_] : 0; }
  int ask_qty() const { return has_ask() ? asks_[best_ask_] : 0; }
  double tick() const { return tick_; }
  // Dividing by an integral inverse gives the double nearest the decimal price
  // (9999 / 100 == 99.99), unlike 9999 * 0.01 == 99.99000000000001.
  double tick_to_price(long long t) const { return static_cast<double>(t) / inv_tick_; }

  // Size resting at `price` on one side (0 if empty or outside the band).
  int quantity(bool bid_side, double price) const {
    long long t;
    if (!price_to_tick(price, t) || t < base_ || t - base_ >= kCapacity) return 0;
    return bid_side ? bids_[t - base_] : asks_[t - base_];
  }

  // Exact price -> tick conversion; false if the price is not on the grid.
  bool price_to_tick(double price, long long& t) const {
    if (!(tick_ > 0) || !std::isfinite(price) || price <= 0) return false;
    const double x = price * inv_tick_;
    if (!(x < 1e12)) return false;
    t = std::llround(x);
    return std::abs(x - static_cast<double>(t)) < 1e-6;
  }

 private:
  bool to_ticks(const std::array<Level, kMaxDepth>& levels, int count,
                std::array<long long, kMaxDepth>& out, long long& low,
                long long& high) {
    for (int i = 0; i < count; ++i) {
      if (levels[i].quantity <= 0) return fail(Reject::kBadSize, levels[i].price);
      if (!price_to_tick(levels[i].price, out[i])) return fail(Reject::kOffGrid, levels[i].price);
      if (out[i] < low) low = out[i];
      if (out[i] > high) high = out[i];
    }
    return true;
  }
  bool fail(Reject why, double price = 0) {
    clear();
    reject_ = why;
    bad_price_ = price;
    return false;
  }
  void clear() {
    for (int i = 0; i < nb_; ++i) bids_[touched_bids_[i]] = 0;
    for (int i = 0; i < na_; ++i) asks_[touched_asks_[i]] = 0;
    nb_ = na_ = 0;
    best_bid_ = best_ask_ = -1;
  }

  // Hot scalars first so the touch reads share one cache line.
  double tick_, inv_tick_;
  long long base_ = 0;
  int best_bid_ = -1, best_ask_ = -1, nb_ = 0, na_ = 0;
  std::array<int, kCapacity> bids_{}, asks_{};
  std::array<int, kMaxDepth> touched_bids_{}, touched_asks_{};
  std::array<long long, kMaxDepth> bid_ticks_{}, ask_ticks_{};
  // Diagnostics for the last rejection (cold).
  Reject reject_ = Reject::kNone;
  double bad_price_ = 0;
  long long span_ = 0;
  int clipped_ = 0;
};

// O(1) incremental signal: EWMA of an imbalance score, no window recompute.
//
// With top-of-book sizes, 2*(microprice - mid)/spread equals
// (bid_qty - ask_qty)/(bid_qty + ask_qty), i.e. the touch imbalance. The venue
// OBI may cover more depth, so the score averages the two: it reduces to the
// touch imbalance when both agree and blends in deeper pressure when not.
struct Signal {
  static constexpr double kAlpha = 0.2;
  double value = 0;
  bool ready = false;
  void update(double microprice, double mid, double spread, double obi) {
    if (!(spread > 0)) return;
    const double x = 0.5 * obi + 0.5 * (2 * (microprice - mid) / spread);
    if (!ready) {
      value = x;
      ready = true;
    } else {
      value += kAlpha * (x - value);
    }
  }
};

// FIFO standing of one resting order, from order_ack / queue_update.
struct QueueState {
  int ahead = 0, level_qty = 0;
  bool known = false;
  void update(int queue_ahead, int qty) {
    ahead = queue_ahead;
    level_qty = qty;
    known = queue_ahead >= 0 && qty > 0;
  }
  // Front quarter of the level: likely to fill soon.
  bool near_front() const { return known && 4 * ahead <= level_qty; }
};

}  // namespace arena
