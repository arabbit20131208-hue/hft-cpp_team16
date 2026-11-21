// quota.hpp — per-interval outbound message budget (venue `order_quota`).
//
// At most `quota` messages per fixed window of `window_ns`. The last
// `cancel_reserve` messages of each window are kept for cancels, so a
// risk-reducing cancel is never starved by new quotes. Strategy thread only.
#pragma once
#include <cstdint>

namespace arena {

class QuotaGuard {
 public:
  QuotaGuard(int quota, long long window_ns, int cancel_reserve = 1)
      : quota_(quota), window_ns_(window_ns), reserve_(cancel_reserve) {}

  // Ask for one message at time `now_ns`. Returns true and counts it, or
  // false (and counts a skip) if the window's budget is spent.
  bool allow(long long now_ns, bool cancel) {
    if (quota_ <= 0) return true;  // disabled
    if (now_ns - start_ns_ >= window_ns_ || now_ns < start_ns_) {
      start_ns_ = now_ns;
      used_ = 0;
    }
    const int limit = cancel ? quota_ : quota_ - reserve_;
    if (used_ >= limit) {
      ++skipped_;
      return false;
    }
    ++used_;
    if (used_ > max_used_) max_used_ = used_;
    return true;
  }

  int quota() const { return quota_; }
  long long window_ns() const { return window_ns_; }
  long long skipped() const { return skipped_; }
  int max_used_in_window() const { return max_used_; }

 private:
  int quota_;
  long long window_ns_;
  int reserve_;
  long long start_ns_ = -(1LL << 62);
  int used_ = 0, max_used_ = 0;
  long long skipped_ = 0;
};

}  // namespace arena
