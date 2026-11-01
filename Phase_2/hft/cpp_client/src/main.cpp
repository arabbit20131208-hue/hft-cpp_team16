#include <atomic>
#include <csignal>
#include <cmath>
#include <memory>
#include "hft_bot.hpp"
class QueueBot : public arena::HFTBot {
public:
  explicit QueueBot(arena::ClientConfig cfg)
    : HFTBot(cfg), tick_(cfg.tick_size) {
    if (!std::isfinite(tick_) || tick_<=0) throw std::runtime_error("Invalid TICK_SIZE");
    for (auto& symbol : symbols_) symbol.reserve(128);
    for (auto& s : states_) {
      s.book=arena::LocalBook(tick_);
      s.buy.id.reserve(128);
      s.sell.id.reserve(128);
    }
  }
private:
  struct Order {
    std::string id;
    bool active=false;
    arena::QueueState queue;
    void clear() { id.clear(); active=false; queue={}; }
  };
  struct State {
    arena::LocalBook book;
    arena::Signal signal;
    Order buy, sell;
    int position=0;
    bool valid=false;
  };
  std::array<std::string,64> symbols_;
  std::array<State,64> states_;
  int count_=0;
  double tick_;
  State* state(const std::string& symbol) {
    for (int i=0; i<count_; ++i)
      if (symbols_[i]==symbol) return &states_[i];
    if (count_==64 || symbol.size()>128) return nullptr;
    symbols_[count_]=symbol;
    return &states_[count_++];
  }
  void on_depth(const arena::BookView& b) override {
    State* s=state(b.symbol);
    if (s) s->valid=s->book.update(b);
  }
  void on_book(const std::string& symbol, double, double, double mid,
        double microprice, double obi) override {
    State* s=state(symbol);
    if (!s || !s->valid) return;
    double bid=s->book.bid(), ask=s->book.ask();
    if (bid<=0 || ask<=bid) return;
    s->signal.update(microprice,mid,ask-bid,obi);
    double value=s->signal.value;
    // Keep an early FIFO position. Require a stronger signal to quote the other side.
    double buy_edge=s->sell.active && s->sell.queue.near_front()?0.50:0.15;
    double sell_edge=s->buy.active && s->buy.queue.near_front()?0.50:0.15;
    if (value>buy_edge && !s->buy.active && s->position<5) {
      s->buy.active=true;
      buy_limit(symbol,1,bid);
    } else if (value < -sell_edge && !s->sell.active && s->position>-5) {
      s->sell.active=true;
      sell_limit(symbol,1,ask);
    }
  }
  void on_order_ack(const std::string& id, const std::string& symbol,
           const std::string& side, double, int,
           int ahead, int level_qty) override {
    State* s=state(symbol);
    if (!s || id.empty() || id.size()>128 || (side!="buy" && side!="sell")) return;
    Order& o=side=="buy"?s->buy:s->sell;
    if (!o.active) return;
    o.id=id;
    o.queue.update(ahead,level_qty);
  }
  void on_queue_update(const std::string& id, const std::string& symbol,
            const std::string& side, double,
            int ahead, int level_qty) override {
    State* s=state(symbol);
    if (!s || (side!="buy" && side!="sell")) return;
    Order& o=side=="buy"?s->buy:s->sell;
    if (o.active && !o.id.empty() && o.id==id) o.queue.update(ahead,level_qty);
  }
  void on_fill(const std::string& side, const std::string& symbol,
        int quantity, double, bool) override {
    State* s=state(symbol);
    if (!s || quantity<=0) return;
    if (side=="buy") { s->position+=quantity; s->buy.clear(); }
    else if (side=="sell") { s->position-=quantity; s->sell.clear(); }
  }
  void on_error(const std::string& code, const std::string& message) override {
    // No order ID in this callback. Stop placing orders until the error is checked.
    std::cerr << "[bot] stopped: " << code << " " << message << "\n";
    stop();
  }
};
namespace {
std::atomic<arena::HFTBot*> bot_ptr{nullptr};
void handle_signal(int) { if (auto* b=bot_ptr.load()) b->stop(); }
}
int main(int argc, char** argv) {
  auto cfg=arena::ClientConfig::from_env();
  bool replay=false;
  for (int i=1; i<argc; ++i) {
    if (std::string(argv[i])=="--replay") replay=true;
    else if (std::string(argv[i])=="--profile") cfg.profile=true;
  }
  // Fixed books are allocated once, outside the callbacks.
  auto bot=std::make_unique<QueueBot>(cfg);
  if (replay) { bot->run_replay(std::cin,std::cout); return 0; }
  bot_ptr.store(bot.get());
  std::signal(SIGINT,handle_signal);
  std::signal(SIGTERM,handle_signal);
  bot->run();
  bot_ptr.store(nullptr);
}
