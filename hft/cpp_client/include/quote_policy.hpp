// quote_policy.hpp — queue-aware hold/cancel decision for one resting order.
//
// Kept free of I/O so it can be unit-tested and reasoned about on its own.
// A requote is a cancel followed by a fresh order at the current touch, which
// joins the BACK of that level's FIFO. So a good queue spot is only given up
// when holding it is worse than losing it.
#pragma once
#include "local_book.hpp"

namespace arena {

struct PolicyParams {
  double entry = 0.15;           // |signal| needed to post a new order
  double strong_adverse = 0.50;  // signal against us: pull regardless of queue
  bool queue_aware = true;       // false = treat every order as far back
};

enum class Action { kHold, kCancel };

enum class Reason {
  kHold,             // signal still supports the order
  kHoldNearFront,    // mildly adverse, but near the front: keep the spot
  kBehindTouch,      // touch moved away from our price: rejoin at the new touch
  kStrongAdverse,    // strongly adverse: adverse selection beats queue value
  kAdverseFarBack,   // mildly adverse and far back: low fill odds, free the slot
};

struct Decision {
  Action action;
  Reason reason;
};

// `edge` is the signal in the order's favour (+signal for a buy, -signal for
// a sell). `behind_touch` means a better price now exists on our side.
inline Decision resting_decision(bool behind_touch, double edge, const QueueState& q,
                                 const PolicyParams& p) {
  if (behind_touch) return {Action::kCancel, Reason::kBehindTouch};
  if (edge < -p.strong_adverse) return {Action::kCancel, Reason::kStrongAdverse};
  if (edge < 0) {
    const bool front = p.queue_aware && q.near_front();
    if (front) return {Action::kHold, Reason::kHoldNearFront};
    return {Action::kCancel, Reason::kAdverseFarBack};
  }
  return {Action::kHold, Reason::kHold};
}

}  // namespace arena
