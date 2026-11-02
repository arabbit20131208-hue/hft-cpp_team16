#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>

#include "hft_bot.hpp"

class SpreadCaptureBot : public arena::HFTBot {
public:
    explicit SpreadCaptureBot(arena::ClientConfig cfg) : HFTBot(std::move(cfg)) {
#if PHASE1_FAST
        for (auto& s : states_) s.symbol.reserve(128);
#endif
    }

private:
    void on_book(const std::string& symbol, double bid, double ask, double mid,
                 double microprice, double obi) override {
        (void)microprice; (void)obi;
        if (bid <= 0.0 || ask <= 0.0) return;

        const double spread=ask - bid;
#if PHASE1_FAST
        State* s=state(symbol);
        if (!s) return;
        double last_mid=s->has_mid?s->last_mid:mid;
        s->last_mid=mid;
        s->has_mid=true;
        int pos=s->position;
#else
        const double last_mid=last_mid_.count(symbol)?last_mid_[symbol]:mid;
        last_mid_[symbol]=mid;
        const int pos=position_[symbol];
#endif
        const int kClip=1;
        const int kMaxPos=5;
        const double kEdge=0.02;

        if (spread < kEdge) return;

        if (mid > last_mid && pos < kMaxPos) {
            buy_limit(symbol, kClip, ask);
        } else if (mid < last_mid && pos > -kMaxPos) {
            sell_limit(symbol, kClip, bid);
        }
    }

    void on_fill(const std::string& side, const std::string& symbol,
                 int quantity, double price, bool maker) override {
        if (side.empty()) return;
#if PHASE1_FAST
        State* s=state(symbol);
        if (!s) return;
        s->position+=(side=="buy")?quantity:-quantity;
        int pos=s->position;
#else
        position_[symbol]+=(side=="buy")?quantity:-quantity;
        int pos=position_[symbol];
#endif
        std::cerr << "[fill] " << (maker ? "MAKER " : "taker ") << side << " "
                  << quantity << " " << symbol << " @ " << price
                  << "  pos=" << pos << "\n";
    }

    void on_session(const std::string& event, const std::string& message) override {
        std::cerr << "[session] " << event << " — " << message << "\n";
        if (event == "SESSION_CLOSED") {
#if PHASE1_FAST
            for (std::size_t i=0; i<state_count_; ++i) {
                const auto& sym=states_[i].symbol;
                int qty=states_[i].position;
#else
            for (auto& [sym, qty] : position_) {
#endif
                if (qty > 0)      sell_market(sym, qty);
                else if (qty < 0) buy_market(sym, -qty);
            }
        }
    }

#if PHASE1_FAST
    struct State {
        std::string symbol;
        double last_mid=0;
        int position=0;
        bool has_mid=false;
    };
    std::array<State,64> states_;
    std::size_t state_count_=0;
    State* state(const std::string& symbol) {
        if (symbol.size()>128) return nullptr;
        for (std::size_t i=0; i<state_count_; ++i)
            if (states_[i].symbol==symbol) return &states_[i];
        if (state_count_==states_.size()) return nullptr;
        State& s=states_[state_count_++];
        s.symbol=symbol;
        return &s;
    }
#else
    std::unordered_map<std::string, int>    position_;
    std::unordered_map<std::string, double> last_mid_;
#endif
};

namespace {
std::atomic<arena::HFTBot*> g_bot{nullptr};
void handle_signal(int) {
    if (auto* b=g_bot.load()) b->stop();
}
}

int main(int argc, char** argv) {
    arena::ClientConfig cfg=arena::ClientConfig::from_env();
    bool replay=false;
    for (int i=1; i < argc; ++i)
        if (std::string(argv[i])=="--replay") replay=true;
        else if (std::string(argv[i])=="--profile") cfg.profile=true;

    SpreadCaptureBot bot(cfg);

    if (replay) {
        bot.run_replay(std::cin, std::cout);
        return 0;
    }

    std::cerr << "[main] AlgoArena HFT bot — team=" << cfg.team_id
              << " url=" << cfg.url << "\n";
    g_bot.store(&bot);
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    bot.run();
    std::cerr << "[main] shutting down\n";
    return 0;
}
