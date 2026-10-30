#include "sip_blf.h"
#ifdef USE_ESPHOME_VOIP_BLF
#include <algorithm>
#include <cstring>
#include <limits>

namespace esphome::voip_stack {
namespace {
constexpr std::string_view NS = "urn:ietf:params:xml:ns:dialog-info";
bool space(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
std::string_view trim(std::string_view value) {
  while (!value.empty() && space(value.front())) value.remove_prefix(1);
  while (!value.empty() && space(value.back())) value.remove_suffix(1);
  return value;
}
std::string_view local_name(std::string_view name) {
  const auto colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}
struct XmlToken {
  enum Kind { OPEN, CLOSE, TEXT, END, ERROR } kind{ERROR};
  std::string_view name, ns, text;
  struct Attribute { std::string_view name, value; };
  std::array<Attribute, 16> attributes{};
  uint8_t count{0}, depth{0};
  std::string_view attr(std::string_view key) const {
    for (uint8_t i = 0; i < this->count; ++i)
      if (this->attributes[i].name == key) return this->attributes[i].value;
    return {};
  }
};
class XmlReader {
 public:
  explicit XmlReader(std::string_view data) : data_(data) {}
  XmlToken::Kind next(XmlToken *token) {
    *token = {};
    if (this->self_close_) {
      this->self_close_ = false;
      token->kind = XmlToken::CLOSE;
      token->name = this->stack_[this->depth_ - 1];
      token->ns = this->resolve_(token->name);
      token->depth = this->depth_;
      this->pop_();
      return token->kind;
    }
    while (this->position_ < this->data_.size()) {
      const size_t start = this->position_;
      if (this->data_[start] != '<') {
        const auto end = this->data_.find('<', start);
        this->position_ = end == std::string_view::npos ? this->data_.size() : end;
        token->text = this->data_.substr(start, this->position_ - start);
        token->kind = XmlToken::TEXT;
        token->depth = this->depth_;
        return token->kind;
      }
      if (this->data_.substr(start, 4) == "<!--") {
        const auto end = this->data_.find("-->", start + 4);
        if (end == std::string_view::npos) return XmlToken::ERROR;
        this->position_ = end + 3;
        continue;
      }
      if (this->data_.substr(start, 2) == "<?") {
        const auto end = this->data_.find("?>", start + 2);
        if (end == std::string_view::npos) return XmlToken::ERROR;
        this->position_ = end + 2;
        continue;
      }
      if (this->data_.substr(start, 9) == "<![CDATA[") {
        const auto end = this->data_.find("]]>", start + 9);
        if (end == std::string_view::npos) return XmlToken::ERROR;
        token->text = this->data_.substr(start + 9, end - start - 9);
        token->kind = XmlToken::TEXT;
        token->depth = this->depth_;
        this->position_ = end + 3;
        return token->kind;
      }
      if (this->data_.substr(start, 2) == "<!") return XmlToken::ERROR;
      this->position_++;
      const bool closing = this->take_('/');
      token->name = this->name_();
      if (token->name.empty()) return XmlToken::ERROR;
      if (closing) {
        this->whitespace_();
        if (!this->take_('>') || this->depth_ == 0 || this->stack_[this->depth_ - 1] != token->name)
          return XmlToken::ERROR;
        token->ns = this->resolve_(token->name);
        token->kind = XmlToken::CLOSE;
        token->depth = this->depth_;
        this->pop_();
        return token->kind;
      }
      if (this->depth_ == this->stack_.size()) return XmlToken::ERROR;
      const uint8_t depth = this->depth_ + 1;
      while (true) {
        const bool separated = this->whitespace_();
        if (this->take_('>')) break;
        if (this->take_('/')) {
          if (!this->take_('>')) return XmlToken::ERROR;
          this->self_close_ = true;
          break;
        }
        if (!separated || token->count == token->attributes.size()) return XmlToken::ERROR;
        const auto name = this->name_();
        this->whitespace_();
        if (name.empty() || !this->take_('=')) return XmlToken::ERROR;
        this->whitespace_();
        if (this->position_ == this->data_.size()) return XmlToken::ERROR;
        const char quote = this->data_[this->position_++];
        if (quote != '\'' && quote != '"') return XmlToken::ERROR;
        const size_t value_start = this->position_;
        const auto end = this->data_.find(quote, value_start);
        if (end == std::string_view::npos) return XmlToken::ERROR;
        const auto value = this->data_.substr(value_start, end - value_start);
        if (value.find('<') != std::string_view::npos) return XmlToken::ERROR;
        this->position_ = end + 1;
        for (uint8_t i = 0; i < token->count; ++i)
          if (token->attributes[i].name == name) return XmlToken::ERROR;
        token->attributes[token->count++] = {name, value};
        if (name == "xmlns" || name.substr(0, 6) == "xmlns:") {
          if (this->binding_count_ == this->bindings_.size()) return XmlToken::ERROR;
          this->bindings_[this->binding_count_++] = {name == "xmlns" ? std::string_view{} : name.substr(6), value, depth};
        }
      }
      token->kind = XmlToken::OPEN;
      token->ns = this->resolve_(token->name);
      token->depth = depth;
      this->stack_[this->depth_++] = token->name;
      return token->kind;
    }
    return this->depth_ == 0 ? XmlToken::END : XmlToken::ERROR;
  }
 private:
  struct Binding { std::string_view prefix, uri; uint8_t depth; };
  std::string_view data_;
  size_t position_{0};
  std::array<std::string_view, 16> stack_{};
  std::array<Binding, 32> bindings_{};
  uint8_t depth_{0}, binding_count_{0};
  bool self_close_{false};
  bool take_(char c) {
    if (this->position_ == this->data_.size() || this->data_[this->position_] != c) return false;
    ++this->position_; return true;
  }
  bool whitespace_() {
    const auto start = this->position_;
    while (this->position_ < this->data_.size() && space(this->data_[this->position_])) ++this->position_;
    return start != this->position_;
  }
  std::string_view name_() {
    const auto start = this->position_;
    while (this->position_ < this->data_.size()) {
      const char c = this->data_[this->position_];
      if (space(c) || c == '/' || c == '>' || c == '=') break;
      if (c == '<' || c == '\'' || c == '"' || c == '?' || c == '!') return {};
      ++this->position_;
    }
    return this->data_.substr(start, this->position_ - start);
  }
  std::string_view resolve_(std::string_view name) const {
    const auto colon = name.find(':');
    const auto prefix = colon == std::string_view::npos ? std::string_view{} : name.substr(0, colon);
    for (uint8_t i = this->binding_count_; i > 0; --i)
      if (this->bindings_[i - 1].prefix == prefix) return this->bindings_[i - 1].uri;
    return {};
  }
  void pop_() {
    while (this->binding_count_ != 0 && this->bindings_[this->binding_count_ - 1].depth == this->depth_)
      --this->binding_count_;
    --this->depth_;
  }
};

template<size_t N> bool decode(std::string_view input, std::array<char, N> *output) {
  size_t count = 0;
  const auto append = [&](char c) {
    if (c == 0 || static_cast<unsigned char>(c) < 32 || count + 1 >= N) return false;
    (*output)[count++] = c; return true;
  };
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '&') { if (!append(input[i])) return false; continue; }
    const auto end = input.find(';', i + 1);
    if (end == std::string_view::npos || end - i > 12) return false;
    const auto entity = input.substr(i + 1, end - i - 1);
    char c = 0;
    if (entity == "amp") c = '&'; else if (entity == "lt") c = '<'; else if (entity == "gt") c = '>';
    else if (entity == "quot") c = '"'; else if (entity == "apos") c = '\'';
    else if (!entity.empty() && entity.front() == '#') {
      size_t at = 1; unsigned value = 0, base = 10;
      if (at < entity.size() && entity[at] == 'x') { ++at; base = 16; }
      if (at == entity.size()) return false;
      for (; at < entity.size(); ++at) {
        const char digit = entity[at];
        const unsigned n = digit >= '0' && digit <= '9' ? digit - '0' :
                           digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
                           digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : 16;
        if (n >= base || value > (0x10FFFFU - n) / base) return false;
        value = value * base + n;
      }
      if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF) || value == 0xFFFE || value == 0xFFFF)
        return false;
      if (value > 127) {
        if (value <= 0x7FF) {
          if (!append(static_cast<char>(0xC0 | (value >> 6))) || !append(static_cast<char>(0x80 | (value & 63)))) return false;
        } else if (value <= 0xFFFF) {
          if (!append(static_cast<char>(0xE0 | (value >> 12))) || !append(static_cast<char>(0x80 | ((value >> 6) & 63))) ||
              !append(static_cast<char>(0x80 | (value & 63)))) return false;
        } else {
          if (!append(static_cast<char>(0xF0 | (value >> 18))) || !append(static_cast<char>(0x80 | ((value >> 12) & 63))) ||
              !append(static_cast<char>(0x80 | ((value >> 6) & 63))) || !append(static_cast<char>(0x80 | (value & 63)))) return false;
        }
        i = end;
        continue;
      }
      c = static_cast<char>(value);
    } else return false;
    if (!append(c)) return false;
    i = end;
  }
  (*output)[count] = 0;
  return true;
}
struct Patch {
  std::array<char, BlfDialogCache::MAX_ID_BYTES + 1> id{};
  BlfState state{BlfState::UNKNOWN};
  bool has_state{false}, terminated{false}, has_rendering{false}, held{false};
};
}  // namespace

BlfDocumentResult BlfDialogCache::apply(std::string_view xml) {
  if (xml.empty() || xml.size() > 4096) return BlfDocumentResult::LIMIT;
  for (const unsigned char ch : xml)
    if (ch == 0 || (ch < 32 && ch != '\r' && ch != '\n' && ch != '\t')) return BlfDocumentResult::INVALID;
  std::array<Patch, MAX_DIALOGS> patches{};
  size_t count = 0;
  bool root = false, complete = false, full = false, collecting_state = false;
  uint8_t local_depth = 0;
  uint32_t version = 0;
  std::array<char, 32> state_text{};
  size_t state_length = 0;
  XmlReader reader(xml);
  XmlToken token;
  while (true) {
    const auto type = reader.next(&token);
    if (type == XmlToken::ERROR) return BlfDocumentResult::INVALID;
    if (type == XmlToken::END) break;
    const auto name = local_name(token.name);
    if (type == XmlToken::TEXT) {
      if (collecting_state) {
        if (state_length + token.text.size() >= state_text.size()) return BlfDocumentResult::INVALID;
        std::memcpy(state_text.data() + state_length, token.text.data(), token.text.size());
        state_length += token.text.size(); state_text[state_length] = 0;
      } else if (token.depth == 0 && !trim(token.text).empty()) return BlfDocumentResult::INVALID;
      continue;
    }
    if (collecting_state && type == XmlToken::OPEN) return BlfDocumentResult::INVALID;
    if (type == XmlToken::OPEN && token.depth == 1) {
      if (root || name != "dialog-info" || token.ns != NS) return BlfDocumentResult::INVALID;
      root = true;
      const auto state = token.attr("state");
      if (state != "full" && state != "partial") return BlfDocumentResult::INVALID;
      full = state == "full";
      const auto entity = token.attr("entity");
      if (entity.empty() || entity.size() > 512) return BlfDocumentResult::INVALID;
      const auto number = trim(token.attr("version"));
      if (number.empty()) return BlfDocumentResult::INVALID;
      for (char c : number) {
        if (c < '0' || c > '9' || version > (std::numeric_limits<uint32_t>::max() - (c - '0')) / 10)
          return BlfDocumentResult::INVALID;
        version = version * 10 + c - '0';
      }
    } else if (type == XmlToken::CLOSE && token.depth == 1) {
      if (name != "dialog-info" || token.ns != NS) return BlfDocumentResult::INVALID;
      complete = true;
    } else if (token.ns == NS && name == "dialog" && token.depth == 2) {
      if (type == XmlToken::OPEN) {
        if (count == MAX_DIALOGS) return BlfDocumentResult::LIMIT;
        if (!decode(token.attr("id"), &patches[count].id) || patches[count].id[0] == 0)
          return BlfDocumentResult::INVALID;
        for (size_t i = 0; i < count; ++i)
          if (patches[i].id == patches[count].id) return BlfDocumentResult::INVALID;
      } else {
        if (full && !patches[count].has_state) return BlfDocumentResult::INVALID;
        ++count;
      }
    } else if (count < MAX_DIALOGS && patches[count].id[0] != 0 && token.ns == NS) {
      if (name == "state" && token.depth == 3) {
        if (type == XmlToken::OPEN) {
          if (patches[count].has_state) return BlfDocumentResult::INVALID;
          collecting_state = true; state_length = 0;
        } else {
          collecting_state = false;
          std::array<char, 32> decoded{};
          if (!decode(trim(std::string_view(state_text.data(), state_length)), &decoded)) return BlfDocumentResult::INVALID;
          const std::string_view value(decoded.data());
          auto &patch = patches[count]; patch.has_state = true;
          if (value == "early") patch.state = BlfState::RINGING;
          else if (value == "confirmed") patch.state = BlfState::ACTIVE;
          else if (value == "terminated") patch.terminated = true;
          else if (value != "trying" && value != "proceeding") return BlfDocumentResult::INVALID;
        }
      } else if (name == "local" && token.depth == 3) {
        local_depth = type == XmlToken::OPEN ? token.depth : 0;
      } else if (name == "param" && type == XmlToken::OPEN && local_depth != 0 && token.depth > local_depth &&
                 token.attr("pname") == "+sip.rendering") {
        patches[count].has_rendering = true;
        patches[count].held = token.attr("pval") == "no";
      }
    }
  }
  if (!root || !complete || collecting_state) return BlfDocumentResult::INVALID;
  if (this->valid_ && version <= this->version_) return BlfDocumentResult::IGNORED;
  if (!full && (!this->valid_ || version != this->version_ + 1)) return BlfDocumentResult::NEED_FULL;
  auto next = full ? std::array<Dialog, MAX_DIALOGS>{} : this->dialogs_;
  for (size_t i = 0; i < count; ++i) {
    const auto &patch = patches[i];
    auto found = std::find_if(next.begin(), next.end(), [&](const Dialog &entry) { return entry.occupied && entry.id == patch.id; });
    if (patch.terminated) { if (found != next.end()) *found = {}; continue; }
    if (found == next.end()) {
      if (!patch.has_state) return BlfDocumentResult::NEED_FULL;
      found = std::find_if(next.begin(), next.end(), [](const Dialog &entry) { return !entry.occupied; });
      if (found == next.end()) return BlfDocumentResult::LIMIT;
      found->id = patch.id; found->occupied = true;
    }
    if (patch.has_state) found->state = patch.state;
    if (patch.has_rendering && (found->state == BlfState::ACTIVE || found->state == BlfState::HELD))
      found->state = patch.held ? BlfState::HELD : BlfState::ACTIVE;
  }
  this->dialogs_ = next; this->version_ = version; this->valid_ = true;
  return BlfDocumentResult::APPLIED;
}

void BlfDialogCache::snapshot(BlfStateInfo *out) const {
  out->available = this->valid_; out->version = this->version_;
  out->ringing_dialogs = out->active_dialogs = out->held_dialogs = 0;
  out->state = this->valid_ ? BlfState::IDLE : BlfState::UNKNOWN;
  if (!this->valid_) return;
  bool unknown = false;
  for (const auto &entry : this->dialogs_) {
    if (!entry.occupied) continue;
    if (entry.state == BlfState::RINGING) ++out->ringing_dialogs;
    else if (entry.state == BlfState::ACTIVE) ++out->active_dialogs;
    else if (entry.state == BlfState::HELD) ++out->held_dialogs;
    else unknown = true;
  }
  out->state = out->ringing_dialogs ? BlfState::RINGING : out->active_dialogs ? BlfState::ACTIVE :
               out->held_dialogs ? BlfState::HELD : unknown ? BlfState::UNKNOWN : BlfState::IDLE;
}

}  // namespace esphome::voip_stack
#endif
