#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include "fast_codec.hpp"

namespace arena {

struct BookView {
    std::string symbol;
    double best_bid=0.0;
    double best_ask=0.0;
    double mid=0.0;
    double spread=0.0;
    double ref=0.0;
    double microprice=0.0;
    double obi=0.0;
    std::string asset_type="equity";

    bool has_bid() const { return best_bid > 0.0; }
    bool has_ask() const { return best_ask > 0.0; }
};

struct ClientConfig {
    std::string team_id;
    std::string url;
    std::string token;
    bool profile=false;
    int level=1;

    static ClientConfig from_env();
};

class ArenaClient {
public:
    using clock=std::chrono::steady_clock;
    using time_point=clock::time_point;

    explicit ArenaClient(ClientConfig cfg);
    virtual ~ArenaClient();

    ArenaClient(const ArenaClient&)=delete;
    ArenaClient& operator=(const ArenaClient&)=delete;

    void run();

    void run_replay(std::istream& in, std::ostream& out);

    void stop();

    void place_limit(const std::string& symbol, const std::string& side,
                     int quantity, double price);

    void place_market(const std::string& symbol, const std::string& side,
                      int quantity);

    void cancel(const std::string& order_id, const std::string& symbol);

    const std::string& team_id()      const { return cfg_.team_id; }
    bool               session_open() const { return session_open_.load(); }

    bool book(const std::string& symbol, BookView& out) const;

protected:
    void begin_tick(const std::string& symbol, time_point received);
    void end_tick();

    virtual void on_book_snapshot(const BookView& /*book*/,
                                  time_point /*recv_time*/) {}

    virtual void on_fill(const std::string& /*side*/, const std::string& /*symbol*/,
                         int /*quantity*/, double /*price*/, bool /*maker*/) {}

    virtual void on_portfolio(double /*cash*/, double /*net_worth*/,
                              double /*realized_pnl*/, double /*unrealized_pnl*/) {}

    virtual void on_session(const std::string& /*event*/, const std::string& /*message*/) {}

    virtual void on_order_ack(const std::string& /*order_id*/, const std::string& /*symbol*/,
                              const std::string& /*side*/, double /*price*/, int /*quantity*/,
                              int /*queue_ahead*/, int /*level_qty*/) {}

    virtual void on_queue_update(const std::string& /*order_id*/, const std::string& /*symbol*/,
                                 const std::string& /*side*/, double /*price*/,
                                 int /*queue_ahead*/, int /*level_qty*/) {}

    virtual void on_error(const std::string& /*code*/, const std::string& /*message*/) {}

    virtual void record_latency(const std::string& symbol, long long micros);

private:
    void connect_handlers();
    void send_handshake();
    void dispatch(const std::string& raw);
    void send_raw(const std::string& json);
    void queue(const Message& message);
    void flush_orders();
    void profile_tick(time_point received);
    long long parse_ns_=0, serialize_ns_=0;
    std::size_t sent_=0;
#if PHASE1_FAST
    FastCodec codec_;
    MessageArena messages_;
    std::array<std::string,16> output_, replay_output_;
    std::size_t replay_count_=0;
    bool in_tick_=false;
    const std::string* tick_symbol_=nullptr;
    time_point tick_time_{};
    BookView snapshot_;
    std::array<BookView,64> fixed_books_;
    std::size_t book_count_=0;
#endif

    ClientConfig cfg_;
    ix::WebSocket ws_;
    std::atomic<bool> session_open_{false};
    std::atomic<bool> running_{false};

    bool replay_mode_=false;
    std::vector<std::string> replay_orders_;

    mutable std::mutex book_mtx_;
    std::map<std::string, BookView> books_;

    std::mutex lat_mtx_;
    long long lat_count_=0;
    long long lat_sum_=0;
    long long lat_min_=0;
    long long lat_max_=0;
};

}
