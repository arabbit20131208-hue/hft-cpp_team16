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

#ifndef PHASE1_FAST
#define PHASE1_FAST 1
#endif

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
    int depth=0, field=0, side=0;
    bool is_book=false, price=false, got_bid=false, got_ask=false;
    bool key(std::string& key) {
        if (depth!=1) return true;
        field=0;
        if (key=="type") field=1;
        else if (key=="symbol") field=2;
        else if (key=="mid_price") field=3;
        else if (key=="bids") field=4;
        else if (key=="asks") field=5;
        return true;
    }
    bool string(std::string& value) {
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
        if (depth==1 && field==3) book.mid=value;
        else if (depth==3 && price) {
            if (side==1) { book.best_bid=value; got_bid=true; }
            else { book.best_ask=value; got_ask=true; }
            price=false;
        }
        return true;
    }
    bool number_integer(std::int64_t n) { return number(static_cast<double>(n)); }
    bool number_unsigned(std::uint64_t n) { return number(static_cast<double>(n)); }
    bool number_float(double n, const std::string&) { return number(n); }
    bool start_object(std::size_t) { ++depth; return true; }
    bool end_object() { --depth; return true; }
    bool start_array(std::size_t) {
        if (depth==1) side=field==4?1:field==5?2:0;
        if (depth==2) price=(side==1 && !got_bid) || (side==2 && !got_ask);
        ++depth;
        return true;
    }
    bool end_array() {
        --depth;
        if (depth==1) side=0;
        price=false;
        return true;
    }
    bool null() { return true; }
    bool boolean(bool) { return true; }
    bool binary(nlohmann::json::binary_t&) { return true; }
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
        BookParser parser{book};
        return nlohmann::json::sax_parse(raw,&parser) && parser.is_book && !book.symbol.empty();
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
