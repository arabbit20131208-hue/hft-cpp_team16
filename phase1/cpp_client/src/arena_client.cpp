#include "arena_client.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>

using json=nlohmann::json;

namespace arena {

namespace {

std::string env_or(const char* key, const std::string& def) {
    const char* v=std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    return std::string(v);
}

}

ClientConfig ClientConfig::from_env() {
    ClientConfig c;
    c.team_id=env_or("TEAM_ID", "hft_alpha");
    c.token=env_or("ARENA_TOKEN", "");

    const std::string url=env_or("EXCHANGE_URL", "");
    if (!url.empty()) {
        c.url=url;
    } else {
        const std::string host=env_or("EXCHANGE_HOST", "localhost");
        const std::string port=env_or("EXCHANGE_PORT", "8765");
        c.url="ws://" + host + ":" + port;
    }

    const std::string lvl=env_or("ARENA_LEVEL", "1");
    try { c.level=std::stoi(lvl); } catch (...) { c.level=1; }
    return c;
}

ArenaClient::ArenaClient(ClientConfig cfg) : cfg_(std::move(cfg)) {
#if PHASE1_FAST
    if (cfg_.team_id.size()>128) throw std::length_error("team_id exceeds 128 bytes");
    snapshot_.symbol.reserve(128);
    snapshot_.asset_type.reserve(128);
    for (auto& b : fixed_books_) { b.symbol.reserve(128); b.asset_type.reserve(128); }
    for (auto& out : output_) out.reserve(4096);
    for (auto& out : replay_output_) out.reserve(4096);
#endif
    ws_.setUrl(cfg_.url);
    ws_.setPingInterval(15);
    ws_.setMaxWaitBetweenReconnectionRetries(3000);
    connect_handlers();
}

ArenaClient::~ArenaClient() {
    ws_.stop();
}

void ArenaClient::connect_handlers() {
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                std::cerr << "[arena] connected to " << cfg_.url
                          << " as " << cfg_.team_id << " (trader)\n";
                session_open_.store(false);
                send_handshake();
                break;

            case ix::WebSocketMessageType::Message:
                dispatch(msg->str);
                break;

            case ix::WebSocketMessageType::Close:
                std::cerr << "[arena] connection closed ("
                          << msg->closeInfo.code << ": "
                          << msg->closeInfo.reason << ") — reconnecting\n";
                session_open_.store(false);
                break;

            case ix::WebSocketMessageType::Error:
                std::cerr << "[arena] socket error: "
                          << msg->errorInfo.reason << " — retrying\n";
                break;

            default:
                break;
        }
    });
}

void ArenaClient::send_handshake() {
    json hs={
        {"type",    "handshake"},
        {"team_id", cfg_.team_id},
        {"role",    "trader"},
        {"level",   cfg_.level},
        {"token",   cfg_.token},
    };
    send_raw(hs.dump());
}

void ArenaClient::send_raw(const std::string& payload) {
    if (replay_mode_) {
#if PHASE1_FAST
        if (replay_count_==replay_output_.size()) throw std::runtime_error("Replay message capacity exceeded");
        replay_output_[replay_count_++]=payload;
#else
        replay_orders_.push_back(payload);
#endif
        return;
    }
    ws_.sendText(payload);
}

void ArenaClient::run_replay(std::istream& in, std::ostream& out) {
    replay_mode_=true;
    session_open_.store(true);
    std::string line;
    line.reserve(1024*1024);
    while (std::getline(in, line)) {
        if (line.empty()) { out << "[]\n"; out.flush(); continue; }
#if PHASE1_FAST
        replay_count_=0;
#else
        replay_orders_.clear();
#endif
        dispatch(line);
        out << "[";
#if PHASE1_FAST
        for (std::size_t i=0; i<replay_count_; ++i) {
            if (i) out<<",";
            out<<replay_output_[i];
        }
#else
        for (std::size_t i=0; i<replay_orders_.size(); ++i) {
            if (i) out<<",";
            out<<replay_orders_[i];
        }
#endif
        out << "]\n";
        out.flush();
    }
}

void ArenaClient::run() {
    running_.store(true);
    ws_.start();
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ws_.stop();
}

void ArenaClient::stop() {
    running_.store(false);
}

void ArenaClient::dispatch(const std::string& raw) {
    const time_point recv_time=clock::now();
    parse_ns_=serialize_ns_=0;
    sent_=0;
#if PHASE1_FAST
    if (codec_.decode(raw,snapshot_)) {
        {
            std::lock_guard<std::mutex> lock(book_mtx_);
            std::size_t i=0;
            while (i<book_count_ && fixed_books_[i].symbol!=snapshot_.symbol) ++i;
            if (i==book_count_) {
                if (book_count_==fixed_books_.size()) return;
                ++book_count_;
            }
            fixed_books_[i]=snapshot_;
        }
        if (cfg_.profile) parse_ns_=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-recv_time).count();
        on_book_snapshot(snapshot_,recv_time);
        profile_tick(recv_time);
        return;
    }
#endif

    json m;
    try {
        m=json::parse(raw);
    } catch (const std::exception& e) {
        std::cerr << "[arena] bad JSON frame: " << e.what() << "\n";
        return;
    }

    const std::string type=m.value("type", "");

    if (type=="book_snapshot") {
#if PHASE1_FAST
        return;
#else
        BookView bv;
        bv.symbol=m.value("symbol", "");
        bv.mid=m.value("mid_price", 0.0);
        bv.spread=m.value("spread", 0.0);
        bv.ref=m.value("ref_price", 0.0);
        bv.microprice=m.value("microprice", 0.0);
        bv.obi=m.value("obi", 0.0);
        bv.asset_type=m.value("asset_type", "equity");
        if (m.contains("bids") && m["bids"].is_array() && !m["bids"].empty())
            bv.best_bid=m["bids"][0][0].get<double>();
        if (m.contains("asks") && m["asks"].is_array() && !m["asks"].empty())
            bv.best_ask=m["asks"][0][0].get<double>();

        {
            std::lock_guard<std::mutex> lk(book_mtx_);
            books_[bv.symbol]=bv;
        }
        if (cfg_.profile) parse_ns_=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-recv_time).count();
        on_book_snapshot(bv,recv_time);
        profile_tick(recv_time);
#endif

    } else if (type == "trade_execution") {
        const std::string sym=m.value("symbol", "");
        const int         qty=m.value("quantity", 0);
        const double      price=m.value("price", 0.0);
        const std::string buyer=m.value("buyer_id", "");
        const std::string seller=m.value("seller_id", "");
        std::string side;
        if (buyer == cfg_.team_id)       side="buy";
        else if (seller == cfg_.team_id) side="sell";
        const bool maker=(m.value("maker_id", "") == cfg_.team_id);
        on_fill(side, sym, qty, price, maker);

    } else if (type == "portfolio_update") {
        on_portfolio(m.value("cash", 0.0), m.value("net_worth", 0.0),
                     m.value("realized_pnl", 0.0), m.value("unrealized_pnl", 0.0));

    } else if (type == "session_event") {
        const std::string ev=m.value("event", "");
        const std::string txt=m.value("message", "");
        if (ev == "SESSION_OPEN" || ev == "SESSION_PREOPEN") {
            session_open_.store(true);
        } else if (ev == "SESSION_CLOSED") {
            session_open_.store(false);
        }
        on_session(ev, txt);

    } else if (type == "order_ack") {
        on_order_ack(m.value("order_id", ""), m.value("symbol", ""),
                     m.value("side", ""), m.value("price", 0.0),
                     m.value("quantity", 0),
                     m.value("queue_ahead", 0), m.value("level_qty", 0));

    } else if (type == "queue_update") {
        on_queue_update(m.value("order_id", ""), m.value("symbol", ""),
                        m.value("side", ""), m.value("price", 0.0),
                        m.value("queue_ahead", 0), m.value("level_qty", 0));

    } else if (type == "error") {
        std::cerr << "[arena] server error " << m.value("code", "")
                  << ": " << m.value("message", "") << "\n";
        on_error(m.value("code", ""), m.value("message", ""));

    }
}

void ArenaClient::place_limit(const std::string& symbol, const std::string& side,
                              int quantity, double price) {
#if PHASE1_FAST
    PlaceOrder order;
    if (!order.symbol.set(symbol)) return;
    order.buy=side=="buy";
    order.quantity=quantity;
    order.price=price;
    queue(order);
#else
    auto start=cfg_.profile?clock::now():time_point{};
    json o={
        {"type",       "place_order"},
        {"team_id",    cfg_.team_id},
        {"symbol",     symbol},
        {"side",       side},
        {"order_type", "limit"},
        {"price",      price},
        {"quantity",   quantity},
    };
    std::string payload=o.dump();
    if (cfg_.profile) serialize_ns_+=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-start).count();
    ++sent_;
    send_raw(payload);
#endif
}

void ArenaClient::place_market(const std::string& symbol, const std::string& side,
                               int quantity) {
#if PHASE1_FAST
    PlaceOrder order;
    if (!order.symbol.set(symbol)) return;
    order.buy=side=="buy";
    order.quantity=quantity;
    order.market=true;
    queue(order);
#else
    auto start=cfg_.profile?clock::now():time_point{};
    json o={
        {"type",       "place_order"},
        {"team_id",    cfg_.team_id},
        {"symbol",     symbol},
        {"side",       side},
        {"order_type", "market"},
        {"price",      0.0},
        {"quantity",   quantity},
    };
    std::string payload=o.dump();
    if (cfg_.profile) serialize_ns_+=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-start).count();
    ++sent_;
    send_raw(payload);
#endif
}

void ArenaClient::cancel(const std::string& order_id, const std::string& symbol) {
#if PHASE1_FAST
    CancelOrder order;
    if (!order.symbol.set(symbol) || !order.order_id.set(order_id)) return;
    queue(order);
#else
    auto start=cfg_.profile?clock::now():time_point{};
    json c={
        {"type",     "cancel_order"},
        {"team_id",  cfg_.team_id},
        {"order_id", order_id},
        {"symbol",   symbol},
    };
    std::string payload=c.dump();
    if (cfg_.profile) serialize_ns_+=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-start).count();
    ++sent_;
    send_raw(payload);
#endif
}

void ArenaClient::begin_tick(const std::string& symbol, time_point received) {
#if PHASE1_FAST
    messages_.reset();
    tick_symbol_=&symbol;
    tick_time_=received;
    in_tick_=true;
#else
    (void)symbol; (void)received;
#endif
}

void ArenaClient::queue(const Message& message) {
#if PHASE1_FAST
    std::size_t i=messages_.offset;
    if (!messages_.push(message)) return;
    auto start=cfg_.profile?clock::now():time_point{};
    codec_.encode(messages_.slots[i],cfg_.team_id,output_[i]);
    if (cfg_.profile) serialize_ns_+=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-start).count();
    if (!in_tick_) flush_orders();
#else
    (void)message;
#endif
}

void ArenaClient::flush_orders() {
#if PHASE1_FAST
    for (std::size_t i=0; i<messages_.offset; ++i) {
        send_raw(output_[i]);
        ++sent_;
        if (in_tick_ && std::holds_alternative<PlaceOrder>(messages_.slots[i])) {
            auto us=std::chrono::duration_cast<std::chrono::microseconds>(clock::now()-tick_time_).count();
            record_latency(*tick_symbol_,us);
        }
    }
    messages_.reset();
#endif
}

void ArenaClient::end_tick() {
#if PHASE1_FAST
    flush_orders();
    in_tick_=false;
#endif
}

void ArenaClient::profile_tick(time_point received) {
    if (!cfg_.profile) return;
    auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-received).count();
    std::cerr<<"STAGE "<<parse_ns_<<" "<<serialize_ns_<<" "<<ns<<" "<<sent_<<"\n";
}

bool ArenaClient::book(const std::string& symbol, BookView& out) const {
    std::lock_guard<std::mutex> lk(book_mtx_);
#if PHASE1_FAST
    for (std::size_t i=0; i<book_count_; ++i) {
        if (fixed_books_[i].symbol==symbol) { out=fixed_books_[i]; return true; }
    }
    return false;
#else
    auto it=books_.find(symbol);
    if (it == books_.end()) return false;
    out=it->second;
    return true;
#endif
}

void ArenaClient::record_latency(const std::string& symbol, long long micros) {
    std::lock_guard<std::mutex> lk(lat_mtx_);
    if (lat_count_ == 0) {
        lat_min_=lat_max_=micros;
    } else {
        lat_min_=std::min(lat_min_, micros);
        lat_max_=std::max(lat_max_, micros);
    }
    lat_sum_ += micros;
    ++lat_count_;

    std::cerr << "LAT " << symbol << " " << micros << "\n";
    if (lat_count_ % 50 == 0) {
        std::cerr << "[latency] n=" << lat_count_
                  << " avg=" << (lat_sum_ / lat_count_)
                  << " min=" << lat_min_
                  << " max=" << lat_max_ << " us\n";
    }
}

}
