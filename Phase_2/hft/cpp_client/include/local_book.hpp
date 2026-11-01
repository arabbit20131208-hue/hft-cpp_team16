#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
namespace arena {
constexpr int max_depth=256;
struct Level { double price=0; int quantity=0; };
struct BookDepth {
  std::array<Level,max_depth> bids{}, asks{};
  int bid_count=0, ask_count=0;
};
class LocalBook {
public:
  static constexpr int capacity=4096;
  explicit LocalBook(double tick=0.01) : tick_(tick) {}
  bool update(const BookDepth& s) {
    clear();
    if (s.bid_count<0 || s.bid_count>max_depth ||
      s.ask_count<0 || s.ask_count>max_depth) return false;
    long long low=std::numeric_limits<long long>::max();
    long long high=0;
    for (int side=0; side<2; ++side) {
      const auto& levels=side==0?s.bids:s.asks;
      int count=side==0?s.bid_count:s.ask_count;
      for (int i=0; i<count; ++i) {
        long long t;
        if (!to_tick(levels[i].price,t) || levels[i].quantity<=0) return false;
        if (t<low) low=t;
        if (t>high) high=t;
      }
    }
    if (low==std::numeric_limits<long long>::max()) return true;
    if (high-low>=capacity) return false;
    base_=low;
    for (int i=0; i<s.bid_count; ++i) {
      long long t=std::llround(s.bids[i].price/tick_);
      int index=static_cast<int>(t-base_);
      if (bids_[index]!=0) { clear(); return false; }
      bids_[index]=s.bids[i].quantity;
      old_bids_[nb_++]=index;
      if (best_bid_<index) best_bid_=index;
    }
    for (int i=0; i<s.ask_count; ++i) {
      long long t=std::llround(s.asks[i].price/tick_);
      int index=static_cast<int>(t-base_);
      if (asks_[index]!=0) { clear(); return false; }
      asks_[index]=s.asks[i].quantity;
      old_asks_[na_++]=index;
      if (best_ask_==-1 || best_ask_>index) best_ask_=index;
    }
    return true;
  }
  double bid() const { return best_bid_<0?0:(base_+best_bid_)*tick_; }
  double ask() const { return best_ask_<0?0:(base_+best_ask_)*tick_; }
  int quantity(bool buy, double price) const {
    long long t;
    if (!to_tick(price,t) || t<base_ || t-base_>=capacity) return 0;
    return buy?bids_[t-base_]:asks_[t-base_];
  }
private:
  bool to_tick(double price, long long& t) const {
    if (!std::isfinite(tick_) || tick_<=0 || !std::isfinite(price) || price<=0) return false;
    double x=price/tick_;
    if (!std::isfinite(x) || x>1e12) return false;
    t=std::llround(x);
    return std::abs(x-t)<1e-5;
  }
  void clear() {
    for (int i=0; i<nb_; ++i) bids_[old_bids_[i]]=0;
    for (int i=0; i<na_; ++i) asks_[old_asks_[i]]=0;
    nb_=na_=0;
    best_bid_=best_ask_=-1;
  }
  double tick_;
  long long base_=0;
  std::array<int,capacity> bids_{}, asks_{};
  std::array<int,max_depth> old_bids_{}, old_asks_{};
  int nb_=0, na_=0, best_bid_=-1, best_ask_=-1;
};
struct Signal {
  double value=0;
  bool ready=false;
  void update(double microprice, double mid, double spread, double obi) {
    if (spread<=0) return;
    double x=0.5*obi+0.5*(2*(microprice-mid)/spread);
    if (!ready) { value=x; ready=true; }
    else value+=0.2*(x-value);
  }
};
struct QueueState {
  int ahead=0, level_qty=0;
  bool known=false;
  void update(int a, int q) { ahead=a; level_qty=q; known=a>=0 && q>0; }
  bool near_front() const { return known && ahead<=level_qty/4; }
};
}
