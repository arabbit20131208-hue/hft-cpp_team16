#pragma once
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>
#include <nlohmann/json.hpp>

#ifndef PHASE1_FAST
#define PHASE1_FAST 1
#endif

namespace arena {
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
    template<class T> bool push(const T& m) {
        if (offset==slots.size()) return false;
        slots[offset++]=m;
        return true;
    }
    void reset() { offset=0; }
};

// Walk the venue's JSON without storing arrays or objects.
struct Fields {
    std::string_view s;
    std::size_t pos=0;
    void space() {
        while (pos<s.size() && (s[pos]==' ' || s[pos]=='\n' || s[pos]=='\r' || s[pos]=='\t')) ++pos;
    }
    bool take(char c) {
        space();
        if (pos==s.size() || s[pos]!=c) return false;
        ++pos;
        return true;
    }
    bool quoted(std::string_view& out) {
        if (!take('"')) return false;
        std::size_t start=pos;
        while (pos<s.size()) {
            char c=s[pos++];
            if (c=='"') { out=s.substr(start,pos-start-1); return true; }
            if (c=='\\') {
                if (pos==s.size()) return false;
                ++pos;
            } else if (static_cast<unsigned char>(c)<32) return false;
        }
        return false;
    }
    bool value(int depth=0) {
        space();
        if (pos==s.size() || depth>64) return false;
        char c=s[pos];
        std::string_view ignored;
        if (c=='"') return quoted(ignored);
        if (c=='[' || c=='{') {
            ++pos;
            char close=c=='['?']':'}';
            if (take(close)) return true;
            do {
                if (c=='{' && (!quoted(ignored) || !take(':'))) return false;
                if (!value(depth+1)) return false;
                if (take(close)) return true;
            } while (take(','));
            return false;
        }
        std::size_t start=pos;
        while (pos<s.size() && s[pos]!=',' && s[pos]!=']' && s[pos]!='}' &&
               s[pos]!=' ' && s[pos]!='\n' && s[pos]!='\r' && s[pos]!='\t') ++pos;
        auto token=s.substr(start,pos-start);
        if (token=="null" || token=="true" || token=="false") return true;
        std::size_t i=0;
        auto digits=[&] {
            std::size_t start=i;
            while (i<token.size() && token[i]>='0' && token[i]<='9') ++i;
            return i>start;
        };
        if (!token.empty() && token[0]=='-') ++i;
        if (i==token.size()) return false;
        if (token[i]=='0') ++i;
        else if (token[i]>='1' && token[i]<='9') digits();
        else return false;
        if (i<token.size() && token[i]=='.') { ++i; if (!digits()) return false; }
        if (i<token.size() && (token[i]=='e' || token[i]=='E')) {
            ++i;
            if (i<token.size() && (token[i]=='+' || token[i]=='-')) ++i;
            if (!digits()) return false;
        }
        return i==token.size();
    }
    template<class F> bool each(F f) {
        if (!take('{')) return false;
        if (!take('}')) {
            do {
                std::string_view key;
                if (!quoted(key) || !take(':')) return false;
                space();
                std::size_t start=pos;
                if (!value() || !f(key,s.substr(start,pos-start))) return false;
                if (take('}')) break;
                if (!take(',')) return false;
            } while (true);
        }
        space();
        return pos==s.size();
    }
};
inline double number(std::string_view s) {
    return std::strtod(s.data(),nullptr);
}
inline double best(std::string_view s) {
    Fields f{s};
    if (!f.take('[') || !f.take('[')) return 0;
    f.space();
    return number(s.substr(f.pos));
}
inline bool text(std::string_view s, std::string& out) {
    if (s.size()<2 || s.front()!='"' || s.back()!='"') return false;
    if (s.find('\\')==std::string_view::npos) out.assign(s.data()+1,s.size()-2);
    else {
        try { nlohmann::json::parse(s).get_to(out); }
        catch (...) { return false; }
    }
    return out.size()<=128;
}
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

template<class Derived> struct CodecBase {
    template<class Book> bool decode(const std::string& raw, Book& book) const {
        return static_cast<const Derived&>(*this).read(raw,book);
    }
    void encode(const Message& m, std::string_view team, std::string& out) const {
        out.clear();
        std::visit([&](const auto& value) {
            static_cast<const Derived&>(*this).write(value,team,out);
        },m);
    }
};
struct FastCodec : CodecBase<FastCodec> {
    template<class Book> bool read(const std::string& raw, Book& b) const {
        b.best_bid=b.best_ask=b.mid=b.spread=b.ref=b.microprice=b.obi=0;
        b.symbol.clear();
        b.asset_type="equity";
        bool book=false;
        return Fields{raw}.each([&](auto key, auto value) {
            if (key=="type") book=value=="\"book_snapshot\"";
            else if (key=="symbol") return text(value,b.symbol);
            else if (key=="mid_price") b.mid=number(value);
            else if (key=="bids") b.best_bid=best(value);
            else if (key=="asks") b.best_ask=best(value);
            else if (key=="spread") b.spread=number(value);
            else if (key=="ref_price") b.ref=number(value);
            else if (key=="microprice") b.microprice=number(value);
            else if (key=="obi") b.obi=number(value);
            else if (key=="asset_type") return text(value,b.asset_type);
            return true;
        }) && book && !b.symbol.empty() && std::isfinite(b.mid) &&
           std::isfinite(b.best_bid) && std::isfinite(b.best_ask) &&
           std::isfinite(b.spread) && std::isfinite(b.ref) &&
           std::isfinite(b.microprice) && std::isfinite(b.obi);
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
        auto result=std::to_chars(buffer,buffer+sizeof(buffer),m.quantity);
        out.append(buffer,result.ptr);
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
