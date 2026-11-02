// book_test.cpp — LocalBook vs a std::map reference, codec, signal, policy.
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <random>

#include "fast_codec.hpp"
#include "quote_policy.hpp"

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::abs(a - b) < eps; }

void test_local_book_basics() {
  arena::LocalBook book;
  arena::BookDepth s;
  s.bid_count = 2;
  s.ask_count = 1;
  s.bids[0] = {100, 10};
  s.bids[1] = {99.99, 20};
  s.asks[0] = {100.02, 30};
  assert(book.update(s));
  assert(near(book.bid(), 100) && near(book.ask(), 100.02));
  assert(book.bid_qty() == 10 && book.ask_qty() == 30);
  assert(book.quantity(true, 99.99) == 20);

  // Next snapshot fully replaces the previous one.
  s.bid_count = 1;
  s.bids[0] = {99.98, 15};
  assert(book.update(s));
  assert(book.quantity(true, 100) == 0 && book.quantity(true, 99.99) == 0);
  assert(near(book.bid(), 99.98));

  s = {};
  assert(book.update(s) && !book.has_bid() && !book.has_ask());

  // Rejections leave the book empty.
  s.bid_count = 1;
  s.bids[0] = {100.001, 5};  // off the tick grid
  assert(!book.update(s) && !book.has_bid());
  assert(book.last_reject() == arena::LocalBook::Reject::kOffGrid && book.bad_price() == 100.001);
  s.bids[0] = {100, 5};
  s.ask_count = 1;
  s.asks[0] = {150, 5};  // the spread itself is wider than the band
  assert(!book.update(s) && book.last_reject() == arena::LocalBook::Reject::kSpan);
  assert(book.bad_span() == 5000);
  s.asks[0] = {100.01, 5};
  s.bid_count = 2;
  s.bids[1] = s.bids[0];  // duplicate level
  assert(!book.update(s));
  assert(book.last_reject() == arena::LocalBook::Reject::kDuplicate);
  s.bid_count = 1;
  s.asks[0] = {100, 5};  // locked
  assert(!book.update(s) && !book.has_bid() && !book.has_ask());
  assert(book.last_reject() == arena::LocalBook::Reject::kLocked);
  s.asks[0] = {99.99, 5};  // crossed
  assert(!book.update(s) && book.last_reject() == arena::LocalBook::Reject::kCrossed);
  s.asks[0] = {100.01, 0};  // zero size
  assert(!book.update(s) && book.last_reject() == arena::LocalBook::Reject::kBadSize);
  s.asks[0] = {100.01, 5};
  assert(book.update(s) && book.last_reject() == arena::LocalBook::Reject::kNone);
}

// Deep levels beyond the band are clipped; the touch and nearby depth stay exact.
void test_wide_book_clips_deep_levels() {
  arena::LocalBook book;
  arena::BookDepth s;
  s.bid_count = 3;
  s.ask_count = 2;
  s.bids[0] = {100.00, 10};
  s.bids[1] = {99.50, 20};
  s.bids[2] = {50.00, 30};  // 5,000 ticks below the touch
  s.asks[0] = {100.02, 40};
  s.asks[1] = {160.00, 50};  // 5,998 ticks above
  assert(book.update(s));
  assert(book.last_clipped() == 2);
  assert(near(book.bid(), 100) && near(book.ask(), 100.02));
  assert(book.bid_qty() == 10 && book.ask_qty() == 40);
  assert(book.quantity(true, 99.50) == 20);
  assert(book.quantity(true, 50.00) == 0 && book.quantity(false, 160.00) == 0);

  // One-sided wide book: anchored at its own touch.
  s.ask_count = 0;
  assert(book.update(s) && book.last_clipped() == 1 && near(book.bid(), 100));

  // A narrow snapshot after a wide one is unaffected.
  s.bid_count = 1;
  s.ask_count = 1;
  assert(book.update(s) && book.last_clipped() == 0 && book.quantity(true, 99.50) == 0);
}

void test_against_map_reference() {
  arena::LocalBook book;
  std::mt19937 rng(16);
  for (int k = 0; k < 10000; ++k) {
    arena::BookDepth s;
    std::map<int, int> bids, asks;
    const int center = 10000 + static_cast<int>(rng() % 10000);
    for (int i = 0; i < 20; ++i) {
      const int b = center - i * 2, a = center + 2 + i * 3;
      if (rng() % 4) {
        const int q = 1 + static_cast<int>(rng() % 100);
        bids[b] = q;
        s.bids[s.bid_count++] = {b * .01, q};
      }
      if (rng() % 4) {
        const int q = 1 + static_cast<int>(rng() % 100);
        asks[a] = q;
        s.asks[s.ask_count++] = {a * .01, q};
      }
    }
    assert(book.update(s));
    const double b = bids.empty() ? 0 : bids.rbegin()->first * .01;
    const double a = asks.empty() ? 0 : asks.begin()->first * .01;
    assert(near(book.bid(), b, 1e-8) && near(book.ask(), a, 1e-8));
    for (int i = center - 50; i <= center + 70; ++i) {
      assert(book.quantity(true, i * .01) == (bids.count(i) ? bids[i] : 0));
      assert(book.quantity(false, i * .01) == (asks.count(i) ? asks[i] : 0));
    }
  }
}

void test_codec() {
  arena::BookView v;
  arena::FastCodec codec;
  assert(codec.decode(
      R"({"type":"book_snapshot","symbol":"ABC","mid_price":100.01,"microprice":100.012,)"
      R"("obi":0.2,"bids":[[100,10],[99.99,20]],"asks":[[100.02,30]]})",
      v));
  assert(v.bid_count == 2 && v.bids[1].quantity == 20);
  assert(near(v.microprice, 100.012, 1e-10) && v.obi == .2);
  assert(near(v.best_bid, 100) && near(v.best_ask, 100.02));

  // No microprice/obi (older tapes): derived from the touch.
  assert(codec.decode(
      R"({"type":"book_snapshot","symbol":"ABC","mid_price":100.01,)"
      R"("bids":[[100,30]],"asks":[[100.02,10]]})",
      v));
  assert(near(v.obi, 0.5) && near(v.microprice, (100 * 10 + 100.02 * 30) / 40.0));

  // Malformed depth and non-snapshot messages are refused.
  assert(!codec.decode(R"({"type":"book_snapshot","symbol":"ABC","mid_price":1,"bids":[[100]],"asks":[]})", v));
  assert(!codec.decode(R"({"type":"book_snapshot","symbol":"ABC","mid_price":1,"bids":[[100,1.5]],"asks":[]})", v));
  assert(!codec.decode(R"({"type":"order_ack","order_id":"x","queue_ahead":3})", v));

  // Prices go out in their short form.
  arena::LocalBook book;
  assert(book.tick_to_price(9999) == 99.99);
  std::string out;
  arena::append_number(out, book.tick_to_price(9999));
  assert(out == "99.99");
}

void test_signal() {
  arena::Signal signal;
  signal.update(100.012, 100.01, .02, .2);
  assert(near(signal.value, .2, 1e-8));
  signal.update(100.008, 100.01, .02, -.2);
  assert(near(signal.value, .12, 1e-8));
}

void test_policy() {
  using arena::Action;
  using arena::Reason;
  arena::PolicyParams p;
  arena::QueueState front, back;
  front.update(10, 100);
  back.update(80, 100);
  assert(front.near_front() && !back.near_front());
  arena::QueueState unknown;
  unknown.update(-1, 100);
  assert(!unknown.near_front());

  // Signal supports the order: hold regardless of queue.
  assert(arena::resting_decision(false, 0.3, back, p).action == Action::kHold);
  // Mildly adverse: the queue decides.
  assert(arena::resting_decision(false, -0.2, front, p).reason == Reason::kHoldNearFront);
  assert(arena::resting_decision(false, -0.2, back, p).reason == Reason::kAdverseFarBack);
  // Strongly adverse or behind the touch: cancel regardless.
  assert(arena::resting_decision(false, -0.6, front, p).reason == Reason::kStrongAdverse);
  assert(arena::resting_decision(true, 0.9, front, p).reason == Reason::kBehindTouch);
  // Queue-blind variant treats the front order like a back one.
  p.queue_aware = false;
  assert(arena::resting_decision(false, -0.2, front, p).reason == Reason::kAdverseFarBack);
}

}  // namespace

int main() {
  test_local_book_basics();
  test_wide_book_clips_deep_levels();
  test_against_map_reference();
  test_codec();
  test_signal();
  test_policy();
  std::cout << "book_test: 10000 snapshots matched the map reference; codec, signal and "
               "policy checks passed.\n";
}
