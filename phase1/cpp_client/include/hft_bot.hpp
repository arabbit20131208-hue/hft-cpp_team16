#pragma once

#include <string>

#include "arena_client.hpp"

namespace arena {

class HFTBot : public ArenaClient {
public:
    explicit HFTBot(ClientConfig cfg) : ArenaClient(std::move(cfg)) {}

    void run() { ArenaClient::run(); }

protected:

    virtual void on_book(const std::string& symbol, double bid, double ask,
                         double mid, double microprice, double obi) {
        (void)symbol; (void)bid; (void)ask; (void)mid; (void)microprice; (void)obi;
    }

    virtual void on_fill(const std::string& side, const std::string& symbol,
                         int quantity, double price, bool maker) override {
        (void)side; (void)symbol; (void)quantity; (void)price; (void)maker;
    }

    virtual void on_session(const std::string& event, const std::string& message) override {
        (void)event; (void)message;
    }

    virtual void on_ack(const std::string& symbol, const std::string& order_id,
                        int queue_ahead, int level_qty) {
        (void)symbol; (void)order_id; (void)queue_ahead; (void)level_qty;
    }

    virtual void on_queue(const std::string& symbol, const std::string& order_id,
                          int queue_ahead, int level_qty) {
        (void)symbol; (void)order_id; (void)queue_ahead; (void)level_qty;
    }

    void buy_limit(const std::string& symbol, int qty, double price) {
        stamp_and_send([&] { place_limit(symbol, "buy", qty, price); });
    }
    void sell_limit(const std::string& symbol, int qty, double price) {
        stamp_and_send([&] { place_limit(symbol, "sell", qty, price); });
    }
    void buy_market(const std::string& symbol, int qty) {
        stamp_and_send([&] { place_market(symbol, "buy", qty); });
    }
    void sell_market(const std::string& symbol, int qty) {
        stamp_and_send([&] { place_market(symbol, "sell", qty); });
    }
    void cancel_order(const std::string& order_id, const std::string& symbol) {
        cancel(order_id, symbol);
    }

private:
    void on_book_snapshot(const BookView& bv, time_point recv_time) override {
#if PHASE1_FAST
        begin_tick(bv.symbol,recv_time);
        if (session_open()) on_book(bv.symbol,bv.best_bid,bv.best_ask,bv.mid,bv.microprice,bv.obi);
        end_tick();
#else
        tick_recv_=recv_time;
        tick_symbol_=bv.symbol;
        in_book_call_=true;
        if (session_open()) {
            on_book(bv.symbol, bv.best_bid, bv.best_ask, bv.mid,
                    bv.microprice, bv.obi);
        }
        in_book_call_=false;
#endif
    }

    void on_order_ack(const std::string& order_id, const std::string& symbol,
                      const std::string& /*side*/, double /*price*/, int /*quantity*/,
                      int queue_ahead, int level_qty) override {
        on_ack(symbol, order_id, queue_ahead, level_qty);
    }
    void on_queue_update(const std::string& order_id, const std::string& symbol,
                         const std::string& /*side*/, double /*price*/,
                         int queue_ahead, int level_qty) override {
        on_queue(symbol, order_id, queue_ahead, level_qty);
    }

    template <typename SendFn>
    void stamp_and_send(SendFn&& send) {
#if PHASE1_FAST
        send();
#else
        if (in_book_call_) {
            send();
            const auto now=clock::now();
            const auto micros=std::chrono::duration_cast<std::chrono::microseconds>(
                                    now - tick_recv_).count();
            record_latency(tick_symbol_, static_cast<long long>(micros));
        } else {
            send();
        }
#endif
    }

    time_point   tick_recv_{};
    std::string  tick_symbol_;
    bool       in_book_call_=false;
};

}
