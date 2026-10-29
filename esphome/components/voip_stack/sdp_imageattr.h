#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace esphome::voip_stack {

// Bounded JPEG subset of RFC6236. No persistent lists, allocations or pixel
// processing. Matching uses square pixels and the local directional envelope.
struct ImageAttributeMatch {
  bool valid{false};
  bool explicit_payload{false};
  bool send_present{false}, recv_present{false};
  bool send_compatible{true}, recv_compatible{true};
};

class ImageAttributeParser {
 public:
  static ImageAttributeMatch parse(std::string_view value, uint16_t receive_width, uint16_t receive_height,
                                    uint16_t send_width, uint16_t send_height) {
    ImageAttributeParser parser(value);
    ImageAttributeMatch result;
    if (value.size() > 2048) return result;
    if (!parser.take_('*')) {
      uint32_t pt = 0;
      if (!parser.number_(pt, 127) || pt != 26) return result;
      result.explicit_payload = true;
    }
    if (!parser.space_()) return result;
    for (unsigned direction = 0; direction < 2 && parser.pos_ < value.size(); ++direction) {
      const bool send = parser.word_("send");
      if (!send && !parser.word_("recv")) return {};
      bool &present = send ? result.send_present : result.recv_present;
      bool &compatible = send ? result.send_compatible : result.recv_compatible;
      if (present || !parser.space_()) return {};
      present = true;
      if (parser.take_('*')) {
        compatible = true;
      } else {
        compatible = false;
        unsigned sets = 0;
        do {
          bool fits = false;
          if (++sets > 8 || !parser.set_(send ? receive_width : send_width,
                                        send ? receive_height : send_height, !send, fits)) return {};
          compatible = compatible || fits;
          parser.spaces_();
        } while (parser.peek_() == '[');
      }
      parser.spaces_();
    }
    if (parser.pos_ != value.size() || (!result.send_present && !result.recv_present)) return {};
    result.valid = true;
    return result;
  }

 private:
  explicit ImageAttributeParser(std::string_view value) : value_(value) {}
  char peek_() const { return this->pos_ < this->value_.size() ? this->value_[this->pos_] : '\0'; }
  bool take_(char value) { if (this->peek_() != value) return false; ++this->pos_; return true; }
  void spaces_() { while (this->peek_() == ' ' || this->peek_() == '\t') ++this->pos_; }
  bool space_() { const size_t before = this->pos_; this->spaces_(); return this->pos_ != before; }
  bool word_(std::string_view word) {
    if (this->value_.substr(this->pos_, word.size()) != word) return false;
    this->pos_ += word.size(); return true;
  }
  bool number_(uint32_t &out, uint32_t maximum = 999999) {
    const size_t before = this->pos_; out = 0;
    while (this->peek_() >= '0' && this->peek_() <= '9') {
      const uint32_t digit = this->peek_() - '0';
      if (out > maximum / 10 || (out == maximum / 10 && digit > maximum % 10)) return false;
      out = out * 10 + digit; ++this->pos_;
    }
    return this->pos_ > before;
  }
  static bool fits_(uint32_t value, uint16_t local, bool exact) {
    return local != 0 && (exact ? value == local : value >= 8 && value <= local && value % 8 == 0);
  }
  bool range_(uint16_t local, bool exact, bool &fits) {
    const bool bracket = this->take_('['); uint32_t first = 0;
    if (!this->number_(first) || first == 0) return false;
    if (!bracket) { fits = fits_(first, local, exact); return true; }
    if (this->take_(':')) {
      uint32_t last = 0, step = 1;
      if (!this->number_(last) || last == 0) return false;
      if (this->take_(':')) { step = last; if (!this->number_(last) || last == 0) return false; }
      if (last <= first || !this->take_(']')) return false;
      if (exact) {
        fits = local >= first && local <= last && (local - first) % step == 0;
      } else {
        const uint32_t low = first < 8 ? 8 : first;
        uint32_t candidate = first + ((low - first + step - 1) / step) * step;
        fits = false;
        // A step sequence meets the eight-pixel JPEG grid within eight terms,
        // or never meets it. Bound work independently of the remote range.
        for (unsigned i = 0; i < 8 && candidate <= last && candidate <= local; ++i, candidate += step)
          fits = fits || fits_(candidate, local, false);
      }
      return true;
    }
    fits = fits_(first, local, exact); unsigned count = 1;
    while (this->take_(',')) {
      uint32_t value = 0;
      if (++count > 32 || !this->number_(value) || value == 0) return false;
      fits = fits || fits_(value, local, exact);
    }
    return count > 1 && this->take_(']');
  }
  bool decimal_(uint32_t &out) {
    uint32_t integer = 0;
    if (!this->number_(integer, 9) || !this->take_('.')) return false;
    out = integer * 10000; uint32_t scale = 1000; unsigned digits = 0;
    while (this->peek_() >= '0' && this->peek_() <= '9') {
      if (++digits > 4) return false;
      out += (this->peek_() - '0') * scale; scale /= 10; ++this->pos_;
    }
    return digits != 0;
  }
  bool aspect_(bool &square) {
    const bool bracket = this->take_('['); uint32_t first = 0;
    if (!this->decimal_(first) || first < 1000) return false;
    square = first == 10000;
    if (!bracket) return true;
    if (this->take_('-')) {
      uint32_t last = 0;
      if (!this->decimal_(last) || last <= first || !this->take_(']')) return false;
      square = first <= 10000 && last >= 10000; return true;
    }
    unsigned count = 1;
    while (this->take_(',')) {
      uint32_t next = 0;
      if (++count > 32 || !this->decimal_(next) || next <= first) return false;
      first = next; square = square || next == 10000;
    }
    return count > 1 && this->take_(']');
  }
  bool set_(uint16_t width, uint16_t height, bool exact, bool &fits) {
    bool x = false, y = false, square = true, supported = true;
    if (!this->take_('[') || !this->word_("x=") || !this->range_(width, exact, x) ||
        !this->take_(',') || !this->word_("y=") || !this->range_(height, exact, y)) return false;
    uint8_t seen = 0;
    while (this->take_(',')) {
      if (this->word_("sar=")) {
        if ((seen & 1U) || !this->aspect_(square)) return false;
        seen |= 1U;
      } else if (this->word_("q=")) {
        uint32_t quality = 0;
        if ((seen & 2U) || !this->decimal_(quality) || quality > 10000) return false;
        seen |= 2U;
      } else if (this->word_("par=")) {
        // PAR negotiation is outside this small subset; do not claim that an
        // unknown picture-aspect constraint was accepted. Legacy SDP remains.
        bool ignored = false;
        if ((seen & 4U) || !this->aspect_(ignored)) return false;
        seen |= 4U; supported = false;
      } else {
        // Unknown extension: skip a bounded, balanced value conservatively.
        unsigned depth = 0; size_t skipped = 0;
        while (this->pos_ < this->value_.size()) {
          const char value = this->peek_();
          if (depth == 0 && (value == ',' || value == ']')) break;
          if (value == '[' && ++depth > 4) return false;
          if (value == ']' && depth != 0) --depth;
          ++this->pos_; ++skipped;
        }
        if (depth != 0 || skipped == 0) return false;
        // RFC6236 section3.2.10 requires unknown parameters to be ignored.
      }
    }
    if (!this->take_(']')) return false;
    fits = x && y && square && supported; return true;
  }
  std::string_view value_;
  size_t pos_{0};
};

}  // namespace esphome::voip_stack
