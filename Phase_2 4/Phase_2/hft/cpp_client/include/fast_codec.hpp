// fast_codec.hpp — allocation-free BookSnapshot decoder and order encoder.
//
// Decoding uses nlohmann's SAX interface, so no JSON DOM is built. Encoding
// writes the PlaceOrder / CancelOrder wire JSON into reused string buffers.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include "local_book.hpp"

namespace arena {

// One decoded BookSnapshot: venue scalars plus the raw depth.
struct BookView : BookDepth {
  std::string symbol;
  double best_bid = 0.0;
  double best_ask = 0.0;
  double mid = 0.0;         // venue mid_price
  double spread = 0.0;      // venue spread (0 if absent)
  double ref = 0.0;         // venue ref_price
  double microprice = 0.0;  // venue value, or derived from the touch if absent
  double obi = 0.0;         // venue value, or derived from the touch if absent
  bool has_bid() const { return best_bid > 0.0; }
  bool has_ask() const { return best_ask > 0.0; }
};

// Fixed-size text for order messages (no heap).
struct Text {
  char data[128]{};
  std::size_t size = 0;
  bool set(std::string_view s) {
    if (s.size() > sizeof(data)) return false;
    std::memcpy(data, s.data(), s.size());
    size = s.size();
    return true;
  }
  std::string_view view() const { return {data, size}; }
};

struct PlaceOrder {
  Text symbol;
  int quantity = 0;
  double price = 0;
  bool buy = true, market = false;
};
struct CancelOrder {
  Text order_id, symbol;
};
using Message = std::variant<PlaceOrder, CancelOrder>;

// Per-tick message slots; reset after every tick.
struct MessageArena {
  std::array<Message, 16> slots;
  std::size_t offset = 0;
  Message* alloc() { return offset == slots.size() ? nullptr : &slots[offset++]; }
  void reset() { offset = 0; }
};

// SAX handler: visits the fields of one book_snapshot without building a tree.
// Returning false aborts the parse; that is used both for malformed depth and
// to stop early when "type" is not book_snapshot.
struct BookParser {
  enum Field { kNone, kType, kSymbol, kMid, kBids, kAsks, kMicro, kObi, kSpread, kRef };
  BookView& book;
  int depth = 0, side = 0, column = 0;
  Field field = kNone;
  bool is_book = false, saw_bids = false, saw_asks = false;
  bool saw_mid = false, saw_micro = false, saw_obi = false;
  Level level{};
  explicit BookParser(BookView& b) : book(b) {}

  bool key(std::string& k) {
    if (depth != 1) return true;
    field = k == "type"         ? kType
            : k == "symbol"     ? kSymbol
            : k == "mid_price"  ? kMid
            : k == "bids"       ? kBids
            : k == "asks"       ? kAsks
            : k == "microprice" ? kMicro
            : k == "obi"        ? kObi
            : k == "spread"     ? kSpread
            : k == "ref_price"  ? kRef
                                : kNone;
    return true;
  }
  bool string(std::string& value) {
    if (depth == 3 && side) return false;
    if (depth != 1) return true;
    if (field == kType) {
      is_book = value == "book_snapshot";
      return is_book;  // not a snapshot: stop now, caller falls back to the DOM path
    }
    if (field == kSymbol) {
      if (value.size() > 128) return false;
      book.symbol = value;
    }
    return true;
  }
  bool number(double value) {
    if (!std::isfinite(value)) return false;
    if (depth == 1) {
      switch (field) {
        case kMid: book.mid = value; saw_mid = true; break;
        case kMicro: book.microprice = value; saw_micro = true; break;
        case kObi: book.obi = value; saw_obi = true; break;
        case kSpread: book.spread = value; break;
        case kRef: book.ref = value; break;
        default: break;
      }
    } else if (depth == 3 && side) {  // [price, quantity]
      if (column == 0) {
        level.price = value;
      } else if (column == 1) {
        if (value <= 0 || value > 2147483647 || std::floor(value) != value) return false;
        level.quantity = static_cast<int>(value);
      } else {
        return false;
      }
      ++column;
    } else if (depth == 2 && side) {
      return false;
    }
    return true;
  }
  bool number_integer(std::int64_t n) { return number(static_cast<double>(n)); }
  bool number_unsigned(std::uint64_t n) { return number(static_cast<double>(n)); }
  bool number_float(double n, const std::string&) { return number(n); }
  bool start_object(std::size_t) {
    if (side) return false;
    ++depth;
    return true;
  }
  bool end_object() {
    --depth;
    return true;
  }
  bool start_array(std::size_t) {
    if (depth == 1) {
      side = field == kBids ? 1 : field == kAsks ? 2 : 0;
      saw_bids |= side == 1;
      saw_asks |= side == 2;
    } else if (depth == 2 && side) {
      column = 0;
      level = {};
    } else if (side) {
      return false;
    }
    ++depth;
    return true;
  }
  bool end_array() {
    if (depth == 3 && side) {
      if (column != 2 || level.price <= 0) return false;
      int& count = side == 1 ? book.bid_count : book.ask_count;
      if (count == kMaxDepth) return false;
      (side == 1 ? book.bids : book.asks)[count++] = level;
    }
    --depth;
    if (depth == 1) side = 0;
    return true;
  }
  bool null() { return side == 0; }
  bool boolean(bool) { return side == 0; }
  bool binary(nlohmann::json::binary_t&) { return side == 0; }
  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) {
    return false;
  }
};

inline void quote(std::string& out, std::string_view s) {
  constexpr char hex[] = "0123456789abcdef";
  out += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
}

// Shortest of %.15g / %.17g that round-trips, so 99.99 goes out as "99.99"
// rather than "99.989999999999995".
inline void append_number(std::string& out, double x) {
  char buffer[64];
  int n = std::snprintf(buffer, sizeof(buffer), "%.15g", x);
  if (std::strtod(buffer, nullptr) != x) n = std::snprintf(buffer, sizeof(buffer), "%.17g", x);
  out.append(buffer, static_cast<std::size_t>(n));
}

struct FastCodec {
  // Decode a book_snapshot into `book`. Returns false for any other message
  // type or a malformed snapshot.
  bool decode(const std::string& raw, BookView& book) const {
    book.symbol.clear();
    book.best_bid = book.best_ask = book.mid = 0;
    book.microprice = book.obi = book.spread = book.ref = 0;
    book.bid_count = book.ask_count = 0;
    BookParser parser{book};
    const bool ok = nlohmann::json::sax_parse(raw, &parser) && parser.is_book &&
                    parser.saw_bids && parser.saw_asks && parser.saw_mid &&
                    !book.symbol.empty();
    if (!ok) return false;

    int bid_q = 0, ask_q = 0;
    for (int i = 0; i < book.bid_count; ++i)
      if (book.bids[i].price > book.best_bid) {
        book.best_bid = book.bids[i].price;
        bid_q = book.bids[i].quantity;
      }
    for (int i = 0; i < book.ask_count; ++i)
      if (book.best_ask == 0 || book.asks[i].price < book.best_ask) {
        book.best_ask = book.asks[i].price;
        ask_q = book.asks[i].quantity;
      }
    // Older tapes carry no microprice/obi; derive both from the touch.
    if ((!parser.saw_micro || !parser.saw_obi) && bid_q > 0 && ask_q > 0) {
      const double total = bid_q + ask_q;
      if (!parser.saw_micro)
        book.microprice = (book.best_bid * ask_q + book.best_ask * bid_q) / total;
      if (!parser.saw_obi) book.obi = (bid_q - ask_q) / total;
    }
    return true;
  }

  void encode(const Message& message, std::string_view team, std::string& out) const {
    std::visit([&](const auto& value) { write(value, team, out); }, message);
  }
  void write(const PlaceOrder& m, std::string_view team, std::string& out) const {
    out = "{\"type\":\"place_order\",\"team_id\":";
    quote(out, team);
    out += ",\"symbol\":";
    quote(out, m.symbol.view());
    out += m.buy ? ",\"side\":\"buy\"" : ",\"side\":\"sell\"";
    out += m.market ? ",\"order_type\":\"market\",\"price\":"
                    : ",\"order_type\":\"limit\",\"price\":";
    append_number(out, m.price);
    out += ",\"quantity\":";
    char buffer[16];
    const int n = std::snprintf(buffer, sizeof(buffer), "%d", m.quantity);
    out.append(buffer, static_cast<std::size_t>(n));
    out += '}';
  }
  void write(const CancelOrder& m, std::string_view team, std::string& out) const {
    out = "{\"type\":\"cancel_order\",\"team_id\":";
    quote(out, team);
    out += ",\"order_id\":";
    quote(out, m.order_id.view());
    out += ",\"symbol\":";
    quote(out, m.symbol.view());
    out += '}';
  }
};

}  // namespace arena
