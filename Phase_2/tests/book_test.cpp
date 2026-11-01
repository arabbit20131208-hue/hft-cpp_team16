#include "fast_codec.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <random>
int main() {
  arena::LocalBook book;
  arena::BookDepth s;
  s.bid_count=2; s.ask_count=1;
  s.bids[0]={100,10}; s.bids[1]={99.99,20}; s.asks[0]={100.02,30};
  assert(book.update(s));
  assert(std::abs(book.bid()-100)<1e-9);
  assert(book.quantity(true,99.99)==20);
  s.bid_count=1; s.bids[0]={99.98,15};
  assert(book.update(s));
  assert(book.quantity(true,100)==0);
  assert(book.quantity(true,99.99)==0);
  assert(std::abs(book.bid()-99.98)<1e-9);
  s={}; assert(book.update(s)); assert(book.bid()==0 && book.ask()==0);
  s.bid_count=1; s.bids[0]={100.001,5}; assert(!book.update(s));
  s.bids[0]={100,5}; s.ask_count=1; s.asks[0]={150,5}; assert(!book.update(s));
  s.asks[0]={100.01,5}; s.bid_count=2; s.bids[1]=s.bids[0]; assert(!book.update(s));
  std::mt19937 rng(16);
  for (int k=0; k<10000; ++k) {
    s={};
    std::map<int,int> bids,asks;
    int center=10000+int(rng()%10000);
    for (int i=0; i<20; ++i) {
      int b=center-i*2, a=center+2+i*3;
      if (rng()%4) { int q=1+rng()%100; bids[b]=q; s.bids[s.bid_count++]={b*.01,q}; }
      if (rng()%4) { int q=1+rng()%100; asks[a]=q; s.asks[s.ask_count++]={a*.01,q}; }
    }
    assert(book.update(s));
    double b=bids.empty()?0:bids.rbegin()->first*.01;
    double a=asks.empty()?0:asks.begin()->first*.01;
    assert(std::abs(book.bid()-b)<1e-8 && std::abs(book.ask()-a)<1e-8);
    for (int i=center-50; i<=center+70; ++i) {
      assert(book.quantity(true,i*.01)==(bids.count(i)?bids[i]:0));
      assert(book.quantity(false,i*.01)==(asks.count(i)?asks[i]:0));
    }
  }
  arena::BookView v;
  arena::FastCodec codec;
  assert(codec.decode(R"({"type":"book_snapshot","symbol":"ABC","mid_price":100.01,"microprice":100.012,"obi":0.2,"bids":[[100,10],[99.99,20]],"asks":[[100.02,30]]})",v));
  assert(v.bid_count==2 && v.bids[1].quantity==20);
  assert(std::abs(v.microprice-100.012)<1e-10 && v.obi==.2);
  assert(!codec.decode(R"({"type":"book_snapshot","symbol":"ABC","bids":[[100]],"asks":[]})",v));
  assert(!codec.decode(R"({"type":"book_snapshot","symbol":"ABC","bids":[[100,1.5]],"asks":[]})",v));
  arena::Signal signal;
  signal.update(100.012,100.01,.02,.2);
  assert(std::abs(signal.value-.2)<1e-8);
  signal.update(100.008,100.01,.02,-.2);
  assert(std::abs(signal.value-.12)<1e-8);
  arena::QueueState q;
  q.update(10,100); assert(q.near_front());
  q.update(80,100); assert(!q.near_front());
  q.update(-1,100); assert(!q.near_front());
  std::cout << "10000 snapshots matched the map reference. Parser and signal checks passed.\n";
}
