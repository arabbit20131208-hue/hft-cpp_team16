#pragma once
#include "arena_client.hpp"

namespace arena {
class HFTBot : public ArenaClient {
public:
    using ArenaClient::ArenaClient;
protected:
    virtual void on_book(const std::string& symbol, double bid, double ask,
                         double mid, double microprice, double obi)=0;
    void buy_limit(const std::string& s, int qty, double price) {
        send_order(s,"buy",qty,price,false);
    }
    void sell_limit(const std::string& s, int qty, double price) {
        send_order(s,"sell",qty,price,false);
    }
    void buy_market(const std::string& s, int qty) { send_order(s,"buy",qty,0,true); }
    void sell_market(const std::string& s, int qty) { send_order(s,"sell",qty,0,true); }
private:
    void on_book_snapshot(const BookView& b, time_point received) override {
#if PHASE1_FAST
        begin_tick(b.symbol,received);
#else
        tick_time_=received;
        tick_symbol_=b.symbol;
        in_book_=true;
#endif
        if (session_open()) on_book(b.symbol,b.best_bid,b.best_ask,b.mid,b.microprice,b.obi);
#if PHASE1_FAST
        end_tick();
#else
        in_book_=false;
#endif
    }
    void send_order(const std::string& symbol, const std::string& side,
                    int qty, double price, bool market) {
        if (market) place_market(symbol,side,qty);
        else place_limit(symbol,side,qty,price);
#if !PHASE1_FAST
        if (in_book_) {
            auto us=std::chrono::duration_cast<std::chrono::microseconds>(clock::now()-tick_time_).count();
            record_latency(tick_symbol_,us);
        }
#endif
    }
#if !PHASE1_FAST
    time_point tick_time_{};
    std::string tick_symbol_;
    bool in_book_=false;
#endif
};
}
