#include "ldap_protocol.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace esphome::ldap_directory {
namespace {

std::string tlv(uint8_t tag, std::string_view value) {
  std::string result(1, static_cast<char>(tag));
  if (value.size() < 128) result.push_back(static_cast<char>(value.size()));
  else {
    uint8_t bytes[sizeof(size_t)];
    size_t count = 0, length = value.size();
    while (length != 0) { bytes[count++] = static_cast<uint8_t>(length); length >>= 8; }
    result.push_back(static_cast<char>(0x80U | count));
    while (count != 0) result.push_back(static_cast<char>(bytes[--count]));
  }
  result.append(value);
  return result;
}

std::string integer(uint8_t tag, uint32_t value) {
  std::string bytes;
  do { bytes.insert(bytes.begin(), static_cast<char>(value & 0xff)); value >>= 8; } while (value != 0);
  if ((static_cast<uint8_t>(bytes[0]) & 0x80U) != 0) bytes.insert(bytes.begin(), '\0');
  return tlv(tag, bytes);
}

std::string message(uint32_t id, uint8_t tag, const std::string &body) {
  return tlv(0x30, integer(0x02, id) + tlv(tag, body));
}

class Reader {
 public:
  explicit Reader(std::string_view value) : value_(value) {}
  bool next(uint8_t *tag, std::string_view *body) {
    if (this->value_.size() < 2) return false;
    *tag = static_cast<uint8_t>(this->value_[0]);
    size_t size = static_cast<uint8_t>(this->value_[1]), offset = 2;
    if ((size & 0x80U) != 0) {
      const size_t count = size & 0x7fU;
      if (count == 0 || count > 4 || this->value_.size() < offset + count) return false;
      size = 0;
      for (size_t i = 0; i < count; ++i) size = (size << 8) | static_cast<uint8_t>(this->value_[offset++]);
    }
    if (size > this->value_.size() - offset) return false;
    *body = this->value_.substr(offset, size);
    this->value_.remove_prefix(offset + size);
    return true;
  }
  bool field(uint8_t wanted, std::string_view *body) {
    uint8_t actual;
    return this->next(&actual, body) && actual == wanted;
  }
  bool number(uint8_t wanted, uint32_t *value) {
    std::string_view bytes;
    if (!this->field(wanted, &bytes) || bytes.empty() || bytes.size() > 5 ||
        (static_cast<uint8_t>(bytes[0]) & 0x80U) != 0 || (bytes.size() == 5 && bytes[0] != 0)) return false;
    uint64_t result = 0;
    for (const unsigned char c : bytes) result = (result << 8) | c;
    if (result > UINT32_MAX) return false;
    *value = static_cast<uint32_t>(result);
    return true;
  }
  bool empty() const { return this->value_.empty(); }
 protected:
  std::string_view value_;
};

bool bounded_text(Reader &reader, uint8_t tag, std::string *text, size_t limit) {
  std::string_view value;
  if (!reader.field(tag, &value) || value.size() > limit) return false;
  text->assign(value);
  return true;
}

bool equal_fold(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) == std::tolower(y);
  });
}

int hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

class Filter {
 public:
  explicit Filter(const std::string &input) : input_(input) {}
  bool encode(std::string *result) {
    return this->input_.size() <= 1024 && this->one_(result, 0) && this->position_ == this->input_.size();
  }
 protected:
  bool one_(std::string *out, size_t depth) {
    if (depth > 8 || ++this->nodes_ > 64 || this->position_ >= this->input_.size() ||
        this->input_[this->position_++] != '(' || this->position_ >= this->input_.size()) return false;
    const char operation = this->input_[this->position_];
    if (operation == '&' || operation == '|' || operation == '!') {
      ++this->position_;
      std::string children;
      size_t count = 0;
      while (this->position_ < this->input_.size() && this->input_[this->position_] == '(') {
        std::string child;
        if (!this->one_(&child, depth + 1) || ++count > 32) return false;
        children += child;
      }
      if (count == 0 || (operation == '!' && count != 1) || !this->end_()) return false;
      *out = tlv(operation == '&' ? 0xa0 : operation == '|' ? 0xa1 : 0xa2, children);
      return true;
    }
    const size_t start = this->position_;
    while (this->position_ < this->input_.size()) {
      const unsigned char c = this->input_[this->position_];
      if (std::isalnum(c) || c == '-' || c == '.' || c == ';') ++this->position_;
      else break;
    }
    if (this->position_ == start || this->position_ - start > 64 || this->position_ >= this->input_.size()) return false;
    const std::string attribute = this->input_.substr(start, this->position_ - start);
    uint8_t tag = 0xa3;
    if (this->input_[this->position_] != '=') {
      const char comparison = this->input_[this->position_++];
      if (comparison == '>') tag = 0xa5;
      else if (comparison == '<') tag = 0xa6;
      else if (comparison == '~') tag = 0xa8;
      else return false;
    }
    if (this->position_ >= this->input_.size() || this->input_[this->position_++] != '=') return false;
    std::vector<std::string> parts(1);
    bool wildcard = false;
    while (this->position_ < this->input_.size() && this->input_[this->position_] != ')') {
      unsigned char c = this->input_[this->position_++];
      if (c == '\\') {
        if (this->input_.size() - this->position_ < 2) return false;
        const int a = hex(this->input_[this->position_++]), b = hex(this->input_[this->position_++]);
        if (a < 0 || b < 0) return false;
        c = static_cast<unsigned char>((a << 4) | b);
      } else if (c == '*') {
        if (tag != 0xa3 || parts.size() > 32) return false;
        wildcard = true;
        parts.emplace_back();
        continue;
      } else if (c == '(' || c == 0) return false;
      if (parts.back().size() >= 256) return false;
      parts.back().push_back(static_cast<char>(c));
    }
    if (!this->end_()) return false;
    if (!wildcard) *out = tlv(tag, tlv(0x04, attribute) + tlv(0x04, parts[0]));
    else {
      std::string items;
      for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        items += tlv(i == 0 ? 0x80 : i + 1 == parts.size() ? 0x82 : 0x81, parts[i]);
      }
      *out = items.empty() ? tlv(0x87, attribute) : tlv(0xa4, tlv(0x04, attribute) + tlv(0x30, items));
    }
    return true;
  }
  bool end_() {
    return this->position_ < this->input_.size() && this->input_[this->position_++] == ')';
  }
  const std::string &input_;
  size_t position_{0}, nodes_{0};
};
}

std::string Entry::first(const std::string &name) const {
  for (const auto &attribute : this->attributes)
    if (equal_fold(attribute.name, name) && !attribute.values.empty()) return attribute.values[0];
  return {};
}

std::string escape_dn_value(const std::string &value) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = value[i];
    if (c == 0 || c < 0x20 || c == 0x7f) {
      out += '\\'; out += digits[c >> 4]; out += digits[c & 15];
    } else {
      if (c == ',' || c == '+' || c == '"' || c == '\\' || c == '<' || c == '>' || c == ';' ||
          (i == 0 && (c == ' ' || c == '#')) || (i + 1 == value.size() && c == ' ')) out += '\\';
      out += static_cast<char>(c);
    }
  }
  return out;
}

bool valid_filter(const std::string &filter) { std::string encoded; return Filter(filter).encode(&encoded); }

bool encode_bind(uint32_t id, const std::string &dn, const std::string &password, std::string *packet) {
  if (id == 0 || id > INT32_MAX || dn.size() > 1024 || password.size() > 256 || packet == nullptr) return false;
  *packet = message(id, 0x60, integer(0x02, 3) + tlv(0x04, dn) + tlv(0x80, password));
  return true;
}

bool encode_search(uint32_t id, const Search &search, std::string *packet) {
  std::string filter;
  if (packet == nullptr || id == 0 || id > INT32_MAX || search.base_dn.size() > 1024 ||
      search.scope > 2 || search.size_limit == 0 || search.size_limit > 64 || search.time_limit > 60 ||
      search.attributes.size() > 16 || !Filter(search.filter).encode(&filter)) return false;
  std::string attributes;
  for (const auto &attribute : search.attributes) {
    if (attribute.empty() || attribute.size() > 64 || attribute.find_first_of("\r\n\0", 0, 3) != std::string::npos) return false;
    attributes += tlv(0x04, attribute);
  }
  *packet = message(id, 0x63, tlv(0x04, search.base_dn) + integer(0x0a, search.scope) + integer(0x0a, 0) +
      integer(0x02, search.size_limit) + integer(0x02, search.time_limit) + tlv(0x01, std::string(1, '\0')) +
      filter + tlv(0x30, attributes));
  return true;
}

std::string encode_unbind(uint32_t id) { return message(id, 0x42, {}); }

bool decode_message(std::string_view packet, Result *result, size_t max_attributes, size_t max_values) {
  if (result == nullptr || packet.size() > 65536) return false;
  Reader outer(packet);
  std::string_view content, operation;
  if (!outer.field(0x30, &content) || !outer.empty()) return false;
  Reader message(content);
  Result parsed;
  if (!message.number(0x02, &parsed.message_id) || !message.next(&parsed.operation, &operation)) return false;
  // Controls are legal on results, but must be well-framed. This client does
  // not request controls and never interprets referrals or follows URLs.
  if (!message.empty()) { std::string_view controls; if (!message.field(0xa0, &controls) || !message.empty()) return false; }
  Reader body(operation);
  if (parsed.operation == 0x64) {
    std::string_view attributes;
    if (!bounded_text(body, 0x04, &parsed.entry.dn, 1024) || !body.field(0x30, &attributes) || !body.empty()) return false;
    Reader list(attributes);
    while (!list.empty()) {
      if (parsed.entry.attributes.size() >= max_attributes) return false;
      std::string_view item, values;
      if (!list.field(0x30, &item)) return false;
      Reader attribute(item);
      Attribute value;
      if (!bounded_text(attribute, 0x04, &value.name, 64) || !attribute.field(0x31, &values) || !attribute.empty()) return false;
      Reader set(values);
      while (!set.empty()) {
        std::string text;
        if (value.values.size() >= max_values || !bounded_text(set, 0x04, &text, 2048)) return false;
        value.values.push_back(std::move(text));
      }
      parsed.entry.attributes.push_back(std::move(value));
    }
  } else if (parsed.operation == 0x61 || parsed.operation == 0x65) {
    std::string matched, diagnostic;
    if (!body.number(0x0a, &parsed.code) || !bounded_text(body, 0x04, &matched, 1024) ||
        !bounded_text(body, 0x04, &diagnostic, 2048)) return false;
    // Bind SASL credentials and result referrals may occur as optional fields.
    while (!body.empty()) { uint8_t tag; std::string_view extra; if (!body.next(&tag, &extra) || (tag != 0xa3 && tag != 0x87)) return false; }
  } else return false;
  *result = std::move(parsed);
  return true;
}

bool read_message(phone_client::Stream &stream, std::string *packet, size_t limit, uint32_t timeout_ms) {
  if (packet == nullptr || limit < 2 || limit > 65536) return false;
  const uint64_t deadline = phone_client::monotonic_ms() + timeout_ms;
  auto read = [&](uint8_t *data, size_t length) {
    const auto now = phone_client::monotonic_ms();
    return now < deadline && phone_client::read_exact(stream, data, length, static_cast<uint32_t>(deadline - now));
  };
  uint8_t header[6];
  if (!read(header, 2) || header[0] != 0x30) return false;
  size_t length = header[1], header_size = 2;
  if ((length & 0x80U) != 0) {
    const size_t count = length & 0x7fU;
    if (count == 0 || count > 4 || !read(header + 2, count)) return false;
    header_size += count; length = 0;
    for (size_t i = 0; i < count; ++i) length = (length << 8) | header[i + 2];
  }
  if (header_size > limit || length > limit - header_size) return false;
  packet->assign(reinterpret_cast<const char *>(header), header_size);
  packet->resize(header_size + length);
  return read(reinterpret_cast<uint8_t *>(&(*packet)[header_size]), length);
}

bool Client::bind(const std::string &dn, const std::string &password, uint32_t timeout_ms) {
  const uint32_t id = this->next_id_++;
  Result result;
  this->result_code_ = 2;
  if (!encode_bind(id, dn, password, &this->packet_) ||
      !phone_client::write_all(this->stream_, this->packet_, timeout_ms) ||
      !this->receive_(&result, timeout_ms) || result.message_id != id || result.operation != 0x61) return false;
  this->result_code_ = result.code;
  return result.code == 0;
}

bool Client::search(const Search &search, std::vector<Entry> *entries, uint32_t timeout_ms) {
  if (entries == nullptr) return false;
  if (search.size_limit == 0 || search.size_limit > 64) { this->result_code_ = 2; return false; }
  std::vector<Entry> staged;
  staged.reserve(search.size_limit);
  if (!this->search_each(search, [&](Entry &&entry) { staged.push_back(std::move(entry)); return true; }, timeout_ms))
    return false;
  entries->swap(staged);
  return true;
}

bool Client::search_each(const Search &search, const std::function<bool(Entry &&)> &consume, uint32_t timeout_ms) {
  if (!consume) return false;
  const uint32_t id = this->next_id_++;
  this->result_code_ = 2;
  if (!encode_search(id, search, &this->packet_) || !phone_client::write_all(this->stream_, this->packet_, timeout_ms)) return false;
  size_t count = 0;
  const uint64_t deadline = phone_client::monotonic_ms() + timeout_ms;
  for (;;) {
    const uint64_t now = phone_client::monotonic_ms();
    Result result;
    if (now >= deadline || !this->receive_(&result, static_cast<uint32_t>(deadline - now)) || result.message_id != id) return false;
    if (result.operation == 0x64) {
      if (count++ >= search.size_limit) { this->result_code_ = 4; return false; }
      if (!consume(std::move(result.entry))) return false;
    } else if (result.operation == 0x65) {
      this->result_code_ = result.code;
      if (result.code != 0) return false;
      return true;
    } else return false;
  }
}

void Client::unbind(uint32_t timeout_ms) {
  phone_client::write_all(this->stream_, encode_unbind(this->next_id_++), timeout_ms);
}

bool Client::receive_(Result *result, uint32_t timeout_ms) {
  if (this->receive_buffer_ != nullptr) {
    size_t length = 0;
    return read_message(this->stream_, this->receive_buffer_, this->max_packet_, &length, timeout_ms) &&
        decode_message(std::string_view(this->receive_buffer_, length), result);
  }
  return read_message(this->stream_, &this->packet_, this->max_packet_, timeout_ms) && decode_message(this->packet_, result);
}

bool read_message(phone_client::Stream &stream, char *buffer, size_t capacity, size_t *length, uint32_t timeout_ms) {
  if (buffer == nullptr || length == nullptr || capacity < 6 || capacity > 65536) return false;
  const uint64_t deadline = phone_client::monotonic_ms() + timeout_ms;
  auto read = [&](uint8_t *data, size_t count) {
    const auto now = phone_client::monotonic_ms();
    return now < deadline && phone_client::read_exact(stream, data, count, static_cast<uint32_t>(deadline - now));
  };
  auto *bytes = reinterpret_cast<uint8_t *>(buffer);
  if (!read(bytes, 2) || bytes[0] != 0x30) return false;
  size_t body = bytes[1], header = 2;
  if ((body & 0x80U) != 0) {
    const size_t count = body & 0x7fU;
    if (count == 0 || count > 4 || !read(bytes + 2, count)) return false;
    header += count; body = 0;
    for (size_t i = 0; i < count; ++i) body = (body << 8) | bytes[i + 2];
  }
  if (header > capacity || body > capacity - header || !read(bytes + header, body)) return false;
  *length = header + body;
  return true;
}

}  // namespace esphome::ldap_directory
