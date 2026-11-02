// book_bench.cpp — LocalBook vs a std::map book on the same generated tape.
//
// Each sample times a batch of kBatch consecutive (update + touch read +
// signal update) operations and divides by kBatch. A single update takes only
// tens of ns, below or near one tick of some steady_clocks (41.7 ns on Apple
// Silicon), so per-call timing would mostly measure the clock. Batch means
// smooth single-call outliers, so p99 here is a p99 of 32-call averages.
//
// Like the client (FastCodec decodes every frame into one reused BookView),
// each op first copies a compact snapshot into one reused BookDepth, then
// updates the book. A tape of full 8 KB BookDepth objects would not fit in
// cache and would measure memory bandwidth instead of the book.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "local_book.hpp"

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kBatch = 32;
constexpr int kRuns = 7;
volatile double g_sink = 0;

struct MapBook {
  std::map<long long, int> bids, asks;
  bool update(const arena::BookDepth& s) {
    bids.clear();
    asks.clear();
    for (int i = 0; i < s.bid_count; ++i) bids[std::llround(s.bids[i].price * 100)] = s.bids[i].quantity;
    for (int i = 0; i < s.ask_count; ++i) asks[std::llround(s.asks[i].price * 100)] = s.asks[i].quantity;
    return true;
  }
  double bid() const { return bids.empty() ? 0 : bids.rbegin()->first * .01; }
  double ask() const { return asks.empty() ? 0 : asks.begin()->first * .01; }
};

// Random-walk tape: 10 levels per side, mid moves +-1 tick ~20% of snapshots,
// ~10% of deeper levels missing, sizes 1..200.
struct Compact {
  int bid_count = 0, ask_count = 0;
  arena::Level bids[10], asks[10];
};

std::vector<Compact> make_tape(int n) {
  std::mt19937 rng(2026);
  std::vector<Compact> tape(n);
  int bid = 10000;
  for (auto& s : tape) {
    const unsigned r = rng() % 10;
    if (r == 0) --bid;
    else if (r == 1) ++bid;
    const int ask = bid + 1 + static_cast<int>(rng() % 2);
    for (int i = 0; i < 10; ++i) {
      if (i == 0 || rng() % 10) s.bids[s.bid_count++] = {(bid - i) * .01, 1 + static_cast<int>(rng() % 200)};
      if (i == 0 || rng() % 10) s.asks[s.ask_count++] = {(ask + i) * .01, 1 + static_cast<int>(rng() % 200)};
    }
  }
  return tape;
}

template <class Book>
void bench(const char* label, const std::vector<Compact>& tape) {
  Book book;
  arena::Signal signal;
  arena::BookDepth s;  // the reused decode buffer
  auto op = [&](const Compact& c) {
    s.bid_count = c.bid_count;
    s.ask_count = c.ask_count;
    for (int i = 0; i < c.bid_count; ++i) s.bids[i] = c.bids[i];
    for (int i = 0; i < c.ask_count; ++i) s.asks[i] = c.asks[i];
    const bool ok = book.update(s);
    const double bid = book.bid(), ask = book.ask();
    signal.update((bid + ask) * .5 + .002, (bid + ask) * .5, ask - bid, .2);
    g_sink = g_sink + bid + ask + signal.value + ok;
  };
  for (int i = 0; i < 20000; ++i) op(tape[i % tape.size()]);  // warm-up

  std::vector<double> p50s, p99s, rates;
  for (int run = 0; run < kRuns; ++run) {
    std::vector<double> per_op;
    per_op.reserve(tape.size() / kBatch);
    const auto run_start = Clock::now();
    for (std::size_t i = 0; i + kBatch <= tape.size(); i += kBatch) {
      const auto t0 = Clock::now();
      for (int j = 0; j < kBatch; ++j) op(tape[i + j]);
      per_op.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kBatch);
    }
    const double secs = std::chrono::duration<double>(Clock::now() - run_start).count();
    std::sort(per_op.begin(), per_op.end());
    p50s.push_back(per_op[per_op.size() / 2]);
    p99s.push_back(per_op[per_op.size() * 99 / 100]);
    rates.push_back(per_op.size() * kBatch / secs);
  }
  std::sort(p50s.begin(), p50s.end());
  std::sort(p99s.begin(), p99s.end());
  std::sort(rates.begin(), rates.end());
  std::printf("%-5s p50_ns=%.1f p99_ns=%.1f snapshots_per_s=%.0f\n", label, p50s[kRuns / 2],
              p99s[kRuns / 2], rates[kRuns / 2]);
}

}  // namespace

int main() {
  const auto tape = make_tape(64000);
  std::printf("tape=%zu snapshots, 10 levels/side, batch=%d, median of %d runs\n", tape.size(),
              kBatch, kRuns);
  bench<MapBook>("map", tape);
  bench<arena::LocalBook>("flat", tape);
}
