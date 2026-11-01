#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>
#include <nlohmann/json.hpp>
#include "local_book.hpp"
#ifndef PHASE1_FAST
#define PHASE1_FAST 1
#endif
namespace arena {
struct BookView : BookDepth {
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
struct Text {
  char data[128]{};
  std::size_t size=0;
  bool set(std::string_view s) {
    if (s.size()>sizeof(data)) return false;
    std::memcpy(data,s.data(),s.size());
    size=s.size();
    return true;
  }
  std::string_view view() const { return {data,size}; }
};
struct PlaceOrder {
  Text symbol;
  int quantity=0;
  double price=0;
  bool buy=true, market=false;
};
struct CancelOrder { Text order_id, symbol; };
using Message=std::variant<PlaceOrder,CancelOrder>;
struct MessageArena {
  std::array<Message,16> slots;
  std::size_t offset=0;
  Message* alloc() {
    if (offset==slots.size()) return nullptr;
    return &slots[offset++];
  }
  void reset() { offset=0; }
};
// SAX visits fields without creating a JSON object tree.
struct BookParser {
  BookView& book;
  int depth=0, field=0, side=0, column=0;
  bool is_book=false, saw_bids=false, saw_asks=false;
  bool saw_mid=false, saw_micro=false, saw_obi=false;
  Level level{};
  explicit BookParser(BookView& b) : book(b) {}
  bool key(std::string& key) {
    if (depth!=1) return true;
    field=0;
    if (key=="type") field=1;
    else if (key=="symbol") field=2;
    else if (key=="mid_price") field=3;
    else if (key=="bids") field=4;
    else if (key=="asks") field=5;
    else if (key=="microprice") field=6;
    else if (key=="obi") field=7;
    else if (key=="spread") field=8;
    else if (key=="ref_price") field=9;
    return true;
  }
  bool string(std::string& value) {
    if (depth==3 && side) return false;
    if (depth!=1) return true;
    if (field==1) is_book=value=="book_snapshot";
    else if (field==2) {
      if (value.size()>128) return false;
      book.symbol=value;
    }
    return true;
  }
  bool number(double value) {
    if (!std::isfinite(value)) return false;
    if (depth==1) {
      if (field==3) { book.mid=value; saw_mid=true; }
      else if (field==6) { book.microprice=value; saw_micro=true; }
      else if (field==7) { book.obi=value; saw_obi=true; }
      else if (field==8) book.spread=value;
      else if (field==9) book.ref=value;
    } else if (depth==3 && side) {
      if (column==0) level.price=value;
      else if (column==1) {
        if (value<=0 || value>2147483647 || std::floor(value)!=value) return false;
        level.quantity=static_cast<int>(value);
      } else return false;
      ++column;
    } else if (depth==2 && side) return false;
    return true;
  }
  bool number_integer(std::int64_t n) { return number(static_cast<double>(n)); }
  bool number_unsigned(std::uint64_t n) { return number(static_cast<double>(n)); }
  bool number_float(double n, const std::string&) { return number(n); }
  bool start_object(std::size_t) {
    if (side) return false;
    ++depth; return true;
  }
  bool end_object() { --depth; return true; }
  bool start_array(std::size_t) {
    if (depth==1) {
      side=field==4?1:field==5?2:0;
      if (side==1) saw_bids=true;
      if (side==2) saw_asks=true;
    } else if (depth==2 && side) { column=0; level={}; }
    else if (side) return false;
    ++depth; return true;
  }
  bool end_array() {
    if (depth==3 && side) {
      if (column!=2 || level.price<=0) return false;
      int& count=side==1?book.bid_count:book.ask_count;
      if (count==max_depth) return false;
      (side==1?book.bids:book.asks)[count++]=level;
    }
    --depth;
    if (depth==1) side=0;
    return true;
  }
  bool null() { return side==0; }
  bool boolean(bool) { return side==0; }
  bool binary(nlohmann::json::binary_t&) { return side==0; }
  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) {
    return false;
  }
};
inline void quote(std::string& out, std::string_view s) {
  constexpr char hex[]="0123456789abcdef";
  out+='"';
  for (unsigned char c : s) {
    if (c=='"' || c=='\\') { out+='\\'; out+=static_cast<char>(c); }
    else if (c<32) {
      out+="\\u00";
      out+=hex[c>>4];
      out+=hex[c&15];
    } else out+=static_cast<char>(c);
  }
  out+='"';
}
inline void append_number(std::string& out, double x) {
  char buffer[64];
  int n=std::snprintf(buffer,sizeof(buffer),"%.17g",x);
  out.append(buffer,static_cast<std::size_t>(n));
}
struct FastCodec {
  bool decode(const std::string& raw, BookView& book) const {
    book.symbol.clear();
    book.best_bid=book.best_ask=book.mid=0;
    book.microprice=book.obi=book.spread=book.ref=0;
    book.bid_count=book.ask_count=0;
    BookParser parser{book};
    bool ok=nlohmann::json::sax_parse(raw,&parser) && parser.is_book &&
        parser.saw_bids && parser.saw_asks && parser.saw_mid &&
        parser.saw_micro && parser.saw_obi && !book.symbol.empty();
    if (!ok) return false;
    for (int i=0; i<book.bid_count; ++i)
      if (book.bids[i].price>book.best_bid) book.best_bid=book.bids[i].price;
    for (int i=0; i<book.ask_count; ++i)
      if (book.best_ask==0 || book.asks[i].price<book.best_ask) book.best_ask=book.asks[i].price;
    return true;
  }
  void encode(const Message& message, std::string_view team, std::string& out) const {
    std::visit([&](const auto& value) { write(value,team,out); },message);
  }
  void write(const PlaceOrder& m, std::string_view team, std::string& out) const {
    out="{\"type\":\"place_order\",\"team_id\":";
    quote(out,team);
    out+=",\"symbol\":";
    quote(out,m.symbol.view());
    out+=m.buy?",\"side\":\"buy\"":",\"side\":\"sell\"";
    out+=m.market?",\"order_type\":\"market\",\"price\":":",\"order_type\":\"limit\",\"price\":";
    append_number(out,m.price);
    out+=",\"quantity\":";
    char buffer[16];
    int n=std::snprintf(buffer,sizeof(buffer),"%d",m.quantity);
    out.append(buffer,static_cast<std::size_t>(n));
    out+='}';
  }
  void write(const CancelOrder& m, std::string_view team, std::string& out) const {
    out="{\"type\":\"cancel_order\",\"team_id\":";
    quote(out,team);
    out+=",\"order_id\":";
    quote(out,m.order_id.view());
    out+=",\"symbol\":";
    quote(out,m.symbol.view());
    out+='}';
  }
};
}
