#include "local_book.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <vector>
struct MapBook {
  std::map<long long,int> bids,asks;
  bool update(const arena::BookDepth& s) {
    bids.clear(); asks.clear();
    for (int i=0; i<s.bid_count; ++i) bids[std::llround(s.bids[i].price*100)]=s.bids[i].quantity;
    for (int i=0; i<s.ask_count; ++i) asks[std::llround(s.asks[i].price*100)]=s.asks[i].quantity;
    return true;
  }
  double bid() const { return bids.empty()?0:bids.rbegin()->first*.01; }
  double ask() const { return asks.empty()?0:asks.begin()->first*.01; }
};
volatile double checksum=0;
using Clock=std::chrono::steady_clock;
template<class Book> void bench(const char* label, const std::vector<arena::BookDepth>& tape) {
  Book book;
  arena::Signal signal;
  for (int i=0; i<5000; ++i) { book.update(tape[i%tape.size()]); checksum+=book.bid(); }
  std::vector<double> medians,tails,rates;
  for (int run=0; run<7; ++run) {
    std::vector<double> times;
    times.reserve(tape.size());
    for (const auto& s : tape) {
      auto start=Clock::now();
      bool ok=book.update(s);
      double bid=book.bid(),ask=book.ask();
      signal.update((bid+ask)*.5+.002,(bid+ask)*.5,ask-bid,.2);
      checksum+=bid+ask+signal.value+ok;
      auto end=Clock::now();
      times.push_back(std::chrono::duration<double,std::nano>(end-start).count());
    }
    std::sort(times.begin(),times.end());
    medians.push_back(times[times.size()/2]); tails.push_back(times[times.size()*99/100]);
    auto start=Clock::now();
    for (int repeat=0; repeat<20; ++repeat)
      for (const auto& s : tape) {
        book.update(s);
        double bid=book.bid(),ask=book.ask();
        signal.update((bid+ask)*.5+.002,(bid+ask)*.5,ask-bid,.2);
        checksum+=bid+ask+signal.value;
      }
    double seconds=std::chrono::duration<double>(Clock::now()-start).count();
    rates.push_back(20*tape.size()/seconds);
  }
  std::sort(medians.begin(),medians.end()); std::sort(tails.begin(),tails.end()); std::sort(rates.begin(),rates.end());
  std::cout<<label<<" p50_ns="<<medians[3]<<" p99_ns="<<tails[3]<<" snapshots_per_s="<<rates[3]<<"\n";
}
int main() {
  std::vector<arena::BookDepth> tape(2000);
  for (int k=0; k<2000; ++k) {
    auto& s=tape[k]; s.bid_count=s.ask_count=10;
    int center=10000+(k/13)%20;
    for (int i=0; i<10; ++i) {
      s.bids[i]={(center-i)*.01,1+(k+i)%100};
      s.asks[i]={(center+2+i)*.01,1+(2*k+i)%100};
    }
  }
  bench<MapBook>("map",tape);
  bench<arena::LocalBook>("flat",tape);
}
