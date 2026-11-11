// hft_bot.hpp — strategy surface. Subclass HFTBot and override the hooks.
//
// Every hook runs on the strategy thread, so bot state needs no locking.
// on_depth(): every decoded snapshot, with full depth (update the local book).
// on_book():  the decision, once the session is open. Orders sent from any
//             hook are collected per event and handed to the send stage
//             together; book-driven orders are latency-stamped against the
//             snapshot's arrival on the rx thread.
#pragma once
#include "arena_client.hpp"

namespace arena {

class HFTBot : public ArenaClient {
 public:
  using ArenaClient::ArenaClient;

 protected:
  virtual void on_depth(const BookView&) {}
  virtual void on_book(const std::string& symbol, double bid, double ask, double mid,
                       double microprice, double obi) = 0;

  void buy_limit(const std::string& s, int qty, double price) {
    place_limit(s, "buy", qty, price);
  }
  void sell_limit(const std::string& s, int qty, double price) {
    place_limit(s, "sell", qty, price);
  }
  void buy_market(const std::string& s, int qty) { place_market(s, "buy", qty); }
  void sell_market(const std::string& s, int qty) { place_market(s, "sell", qty); }
  void cancel_order(const std::string& order_id, const std::string& s) { cancel(order_id, s); }

 private:
  void on_book_snapshot(const BookView& b) final {
    on_depth(b);
    if (session_open()) on_book(b.symbol, b.best_bid, b.best_ask, b.mid, b.microprice, b.obi);
  }
};

}  // namespace arena
