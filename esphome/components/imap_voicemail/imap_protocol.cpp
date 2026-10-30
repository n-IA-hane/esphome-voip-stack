#include "imap_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

namespace esphome::imap_voicemail {
namespace {
std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
  return value;
}
bool starts(std::string_view value, std::string_view prefix) { return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix; }
bool number(std::string_view text, uint32_t *value, bool nonzero = false) {
  if (text.empty() || text.size() > 10) return false;
  uint64_t result = 0;
  for (const unsigned char c : text) { if (!std::isdigit(c)) return false; result = result * 10 + c - '0'; }
  if (result > UINT32_MAX || (nonzero && result == 0)) return false;
  *value = static_cast<uint32_t>(result); return true;
}
uint32_t time_left(uint64_t deadline) {
  const auto now = phone_client::monotonic_ms();
  return now < deadline ? static_cast<uint32_t>(std::min<uint64_t>(deadline - now, UINT32_MAX)) : 0;
}
const Value *field(const Value &fetch, const std::string &key) {
  for (size_t i = 0; i + 1 < fetch.children.size(); i += 2)
    if (upper(fetch.children[i].text) == key) return &fetch.children[i + 1];
  return nullptr;
}
bool is_fetch(const std::string &text) {
  if (!starts(text, "* ")) return false;
  const size_t end = text.find(' ', 2);
  uint32_t sequence;
  return end != std::string::npos && number(std::string_view(text).substr(2, end - 2), &sequence, true) &&
      starts(upper(text.substr(end + 1)), "FETCH ");
}
bool audio_body(const Value &body, const std::string &section, AudioPart *part, size_t depth) {
  if (depth > 12 || body.kind != Value::Kind::LIST || body.children.empty()) return false;
  if (body.children[0].kind == Value::Kind::LIST) {
    size_t child = 0;
    for (const auto &value : body.children) {
      if (value.kind != Value::Kind::LIST) break;
      const std::string next = section.empty() ? std::to_string(child + 1) : section + "." + std::to_string(child + 1);
      if (audio_body(value, next, part, depth + 1)) return true;
      ++child;
    }
    return false;
  }
  if (body.children.size() < 7 || upper(body.children[0].text) != "AUDIO") return false;
  const std::string subtype = upper(body.children[1].text), encoding = upper(body.children[5].text);
  const bool supported = subtype == "WAV" || subtype == "X-WAV" || subtype == "WAVE" || subtype == "MPEG" ||
      subtype == "MP3" || subtype == "OGG" || subtype == "OPUS" || subtype == "FLAC" || subtype == "X-FLAC";
  uint32_t length;
  if (!supported || !number(body.children[6].text, &length) ||
      (encoding != "BASE64" && encoding != "BINARY" && encoding != "8BIT" && encoding != "7BIT" && encoding != "QUOTED-PRINTABLE")) return false;
  part->section = section.empty() ? "1" : section;
  part->mime = "audio/" + subtype;
  std::transform(part->mime.begin(), part->mime.end(), part->mime.begin(), [](unsigned char c) { return std::tolower(c); });
  part->encoding = encoding; part->encoded_size = length;
  return true;
}
}

bool parse_uid(const std::string &value, uint32_t *uid) { return number(value, uid, true); }

bool parse_content_duration(const std::string &headers, uint32_t *duration) {
  if (duration == nullptr || headers.size() > 4096) return false;
  bool found = false;
  size_t at = 0;
  while (at < headers.size()) {
    size_t end = headers.find("\r\n", at);
    if (end == std::string::npos) return false;
    std::string line = headers.substr(at, end - at); at = end + 2;
    if (line.empty()) break;
    while (at < headers.size() && (headers[at] == ' ' || headers[at] == '\t')) {
      end = headers.find("\r\n", at);
      if (end == std::string::npos) return false;
      line += headers.substr(at, end - at); at = end + 2;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos || upper(line.substr(0, colon)) != "CONTENT-DURATION") continue;
    if (found) return false;
    found = true;
    std::string value = line.substr(colon + 1);
    const size_t first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
    if (first == std::string::npos || !number(std::string_view(value).substr(first, last - first + 1), duration) || *duration > INT32_MAX) return false;
  }
  return found;
}

std::string mailbox_wire_name(const std::string &utf8) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
  std::string result, pending;
  auto flush = [&]() {
    if (pending.empty()) return;
    result += '&'; uint32_t bits = 0; unsigned count = 0;
    for (const unsigned char c : pending) {
      bits = (bits << 8) | c; count += 8;
      while (count >= 6) { count -= 6; result += alphabet[(bits >> count) & 63]; }
    }
    if (count != 0) result += alphabet[(bits << (6 - count)) & 63];
    result += '-'; pending.clear();
  };
  for (size_t i = 0; i < utf8.size();) {
    uint32_t code = static_cast<unsigned char>(utf8[i++]); unsigned continuation = 0;
    if (code >= 0xc2 && code <= 0xdf) { code &= 31; continuation = 1; }
    else if (code >= 0xe0 && code <= 0xef) { code &= 15; continuation = 2; }
    else if (code >= 0xf0 && code <= 0xf4) { code &= 7; continuation = 3; }
    else if (code >= 0x80) return {};
    const unsigned expected = continuation;
    while (continuation-- != 0) {
      if (i == utf8.size() || (static_cast<unsigned char>(utf8[i]) & 0xc0) != 0x80) return {};
      code = (code << 6) | (static_cast<unsigned char>(utf8[i++]) & 63);
    }
    if ((expected == 1 && code < 0x80) || (expected == 2 && code < 0x800) || (expected == 3 && code < 0x10000) ||
        code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) || code < 0x20 || code == 0x7f) return {};
    if (code >= 0x20 && code <= 0x7e) { flush(); if (code == '&') result += "&-"; else result += static_cast<char>(code); }
    else {
      if (code > 0xffff) {
        const uint32_t high = 0xd800 + ((code - 0x10000) >> 10);
        pending += static_cast<char>(high >> 8); pending += static_cast<char>(high); code = 0xdc00 + ((code - 0x10000) & 1023);
      }
      pending += static_cast<char>(code >> 8); pending += static_cast<char>(code);
    }
  }
  flush(); return result;
}

std::string make_message_uri(const std::string &host, uint16_t port, const std::string &folder,
                             uint32_t validity, uint32_t uid) {
  static const char hex[] = "0123456789ABCDEF";
  std::string path;
  for (const unsigned char c : folder) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.') path += static_cast<char>(c);
    else { path += '%'; path += hex[c >> 4]; path += hex[c & 15]; }
  }
  return "imap://" + host + ":" + std::to_string(port) + "/" + path + ";UIDVALIDITY=" + std::to_string(validity) + "/;UID=" + std::to_string(uid);
}

bool parse_message_uri(const std::string &uri, const std::string &host, uint16_t port,
                       const std::string &folder, uint32_t *validity, uint32_t *uid) {
  const std::string marker = ";UIDVALIDITY=", message = "/;UID=";
  const size_t start = uri.find(marker), split = uri.find(message, start == std::string::npos ? 0 : start + marker.size());
  if (start == std::string::npos || split == std::string::npos || uri.size() > 2048 ||
      !number(std::string_view(uri).substr(start + marker.size(), split - start - marker.size()), validity, true) ||
      !number(std::string_view(uri).substr(split + message.size()), uid, true)) return false;
  return uri == make_message_uri(host, port, folder, *validity, *uid);
}

std::string quote(const std::string &value) {
  if (value.size() > 1024 || value.find_first_of("\r\n\0", 0, 3) != std::string::npos) return {};
  std::string result = "\"";
  for (const char c : value) { if (c == '\\' || c == '"') result += '\\'; result += c; }
  return result + '"';
}

std::string value_text(const Value &value, const Response &response, const uint8_t *literal_data) {
  if (value.kind != Value::Kind::LITERAL)
    return value.kind == Value::Kind::ATOM && upper(value.text) == "NIL" ? "" : value.text;
  if (value.literal >= response.count) return {};
  const auto &view = response.literals[value.literal];
  return std::string(reinterpret_cast<const char *>(literal_data + view.offset), view.size);
}

bool parse_values(const Response &response, const uint8_t *literal_data, std::vector<Value> *values) {
  if (values == nullptr || response.text.size() > 32768) return false;
  const std::string &text = response.text;
  size_t position = 0, nodes = 0;
  std::function<bool(std::vector<Value> *, size_t, bool)> parse = [&](std::vector<Value> *out, size_t depth, bool nested) {
    if (depth > 16) return false;
    while (position < text.size()) {
      if (text[position] == ' ' || text[position] == '\t') { ++position; continue; }
      if (text[position] == ')') { ++position; return nested; }
      if (++nodes > 512) return false;
      Value value;
      if (text[position] == '(') {
        ++position; value.kind = Value::Kind::LIST;
        if (!parse(&value.children, depth + 1, true)) return false;
      } else if (text[position] == '"') {
        ++position; value.kind = Value::Kind::STRING; bool complete = false;
        while (position < text.size()) {
          char c = text[position++];
          if (c == '"') { complete = true; break; }
          if (c == '\\') { if (position == text.size()) return false; c = text[position++]; }
          if (value.text.size() >= 4096) return false;
          value.text += c;
        }
        if (!complete) return false;
      } else if (text[position] == '\x01') {
        ++position; const size_t start = position;
        while (position < text.size() && text[position] != '\x02') ++position;
        uint32_t index;
        if (position == text.size() || !number(std::string_view(text).substr(start, position - start), &index) || index >= response.count) return false;
        ++position; value.kind = Value::Kind::LITERAL; value.literal = index;
        const auto &view = response.literals[index];
        if (view.size != 0 && literal_data == nullptr) return false;
        if (view.size <= 1024)
          value.text.assign(reinterpret_cast<const char *>(literal_data + view.offset), view.size);
      } else {
        const size_t start = position;
        size_t bracket = 0; bool quoted = false, escaped = false;
        while (position < text.size()) {
          const char c = text[position];
          if (escaped) { escaped = false; ++position; continue; }
          if (bracket != 0 && c == '\\' && quoted) { escaped = true; ++position; continue; }
          if (bracket != 0 && c == '"') quoted = !quoted;
          if (!quoted) {
            if (c == '[') { if (++bracket > 2) return false; }
            else if (c == ']' && bracket != 0) --bracket;
            else if (bracket == 0 && (c == ' ' || c == '\t' || c == '(' || c == ')')) break;
          }
          ++position;
        }
        if (bracket != 0 || quoted || escaped) return false;
        if (position == start || position - start > 4096) return false;
        value.text = text.substr(start, position - start);
      }
      out->push_back(std::move(value));
    }
    return !nested;
  };
  std::vector<Value> parsed;
  if (!parse(&parsed, 0, false)) return false;
  values->swap(parsed); return true;
}

bool parse_bodystructure(const Value &body, AudioPart *audio) {
  if (audio == nullptr) return false;
  AudioPart candidate;
  if (!audio_body(body, "", &candidate, 0)) return false;
  *audio = std::move(candidate); return true;
}

Client::Client(phone_client::Stream &stream, uint8_t *buffer, size_t capacity)
    : stream_(stream), literal_(buffer), literal_capacity_(capacity) {}

bool Client::exact_(uint8_t *data, size_t length, uint64_t deadline) {
  while (length != 0) {
    if (this->input_at_ != this->input_size_) {
      const size_t count = std::min(length, this->input_size_ - this->input_at_);
      std::copy_n(this->input_.data() + this->input_at_, count, data);
      this->input_at_ += count; data += count; length -= count; continue;
    }
    const uint32_t wait = time_left(deadline);
    if (wait == 0) return false;
    const int count = this->stream_.read(this->input_.data(), this->input_.size(), wait);
    if (count <= 0 || static_cast<size_t>(count) > this->input_.size()) return false;
    this->input_at_ = 0; this->input_size_ = static_cast<size_t>(count);
  }
  return true;
}

bool Client::line_(std::string *line, uint64_t deadline) {
  line->clear();
  while (line->size() < 16384) {
    uint8_t value;
    if (!this->exact_(&value, 1, deadline)) return false;
    if (value == '\n') {
      if (line->empty() || line->back() != '\r') return false;
      line->pop_back(); return true;
    }
    if ((value < 0x20 && value != '\r' && value != '\t') || value == 0x7f) return false;
    line->push_back(static_cast<char>(value));
  }
  return false;
}

bool Client::response_(Response *response, uint32_t timeout_ms) {
  Response result;
  const uint64_t deadline = phone_client::monotonic_ms() + timeout_ms;
  for (;;) {
    std::string line;
    if (!this->line_(&line, deadline)) { this->error_ = "connection"; return false; }
    if (result.text.size() + line.size() > 32768) { this->error_ = "response_limit"; return false; }
    bool quoted = false, escaped = false;
    size_t marker = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
      if (escaped) { escaped = false; continue; }
      if (line[i] == '\\' && quoted) { escaped = true; continue; }
      if (line[i] == '"') quoted = !quoted;
      else if (line[i] == '{' && !quoted) marker = i;
    }
    if (!line.empty() && line.back() == '}' && marker != std::string::npos) {
      std::string count = line.substr(marker + 1, line.size() - marker - 2);
      if (!count.empty() && count.back() == '+') count.pop_back();
      uint32_t size;
      if (!number(count, &size) || result.count >= result.literals.size() ||
          result.bytes > this->literal_capacity_ || size > this->literal_capacity_ - result.bytes || this->literal_ == nullptr) {
        this->error_ = "literal_limit"; return false;
      }
      result.text.append(line, 0, marker);
      result.text += '\x01'; result.text += std::to_string(result.count); result.text += '\x02';
      result.literals[result.count++] = {result.bytes, size};
      if (!this->exact_(this->literal_ + result.bytes, size, deadline)) { this->error_ = "connection"; return false; }
      result.bytes += size;
    } else { result.text += line; break; }
  }
  *response = std::move(result); return true;
}

std::string Client::tag_() {
  char tag[16]; snprintf(tag, sizeof(tag), "V%08lx", static_cast<unsigned long>(this->next_tag_++)); return tag;
}
bool Client::send_(const std::string &value, uint32_t timeout_ms) {
  if (value.size() > 4096 || !phone_client::write_all(this->stream_, value, timeout_ms)) {
    this->error_ = "connection"; return false;
  }
  return true;
}

bool Client::command_(const std::string &command, uint32_t timeout_ms,
                       const std::function<bool(const Response &)> &untagged) {
  if (this->idling_) { this->error_ = "idle_active"; return false; }
  const std::string tag = this->tag_();
  if (!this->send_(tag + " " + command + "\r\n", timeout_ms)) return false;
  const uint64_t deadline = phone_client::monotonic_ms() + timeout_ms;
  for (size_t count = 0; count < 4096; ++count) {
    Response response;
    const uint32_t wait = time_left(deadline);
    if (wait == 0 || !this->response_(&response, wait)) return false;
    if (starts(response.text, tag + " ")) {
      const bool ok = starts(upper(response.text.substr(tag.size() + 1)), "OK ") || upper(response.text.substr(tag.size() + 1)) == "OK";
      if (!ok) this->error_ = "command_rejected";
      return ok;
    }
    if (starts(upper(response.text), "* BYE")) { this->error_ = "server_bye"; return false; }
    if (!starts(response.text, "* ") || (untagged && !untagged(response))) { this->error_ = "malformed_response"; return false; }
  }
  this->error_ = "response_limit"; return false;
}

bool Client::login(const std::string &username, const std::string &password, uint32_t timeout_ms) {
  Response greeting;
  if (!this->response_(&greeting, timeout_ms)) return false;
  const std::string hello = upper(greeting.text);
  if (!starts(hello, "* OK") && !starts(hello, "* PREAUTH")) { this->error_ = "bad_greeting"; return false; }
  if (quote(username).empty() || quote(password).empty()) { this->error_ = "credentials"; return false; }
  if (!starts(hello, "* PREAUTH") && !this->command_("LOGIN " + quote(username) + " " + quote(password), timeout_ms)) {
    this->error_ = "authentication"; return false;
  }
  return this->command_("CAPABILITY", timeout_ms, [&](const Response &response) {
    if (!starts(upper(response.text), "* CAPABILITY ")) return true;
    std::vector<Value> values;
    if (!parse_values(response, this->literal_, &values)) return false;
    for (const auto &value : values) {
      const auto name = upper(value.text);
      this->idle_supported_ |= name == "IDLE"; this->uidplus_ |= name == "UIDPLUS";
    }
    return true;
  });
}

bool Client::select(const std::string &folder, uint32_t timeout_ms) {
  const std::string wire_folder = mailbox_wire_name(folder);
  if (wire_folder.empty() || quote(wire_folder).empty()) return false;
  this->uid_validity_ = 0; this->exists_ = 0;
  const bool ok = this->command_("SELECT " + quote(wire_folder), timeout_ms, [&](const Response &response) {
    const std::string text = upper(response.text);
    const size_t validity = text.find("[UIDVALIDITY ");
    if (validity != std::string::npos) {
      const size_t start = validity + 13, end = text.find(']', start);
      if (end == std::string::npos || !number(std::string_view(text).substr(start, end - start), &this->uid_validity_, true)) return false;
    }
    const size_t end = text.find(' ', 2);
    if (starts(text, "* ") && end != std::string::npos && text.substr(end + 1) == "EXISTS" &&
        !number(std::string_view(text).substr(2, end - 2), &this->exists_)) return false;
    return true;
  });
  if (!ok || this->uid_validity_ == 0) { this->error_ = "uid_validity"; return false; }
  return true;
}

bool Client::catalogue(size_t maximum, size_t page, Mailbox *mailbox, uint32_t timeout_ms) {
  if (mailbox == nullptr || maximum == 0 || maximum > 64 || page > UINT32_MAX / maximum) return false;
  Mailbox result(this->buffer_in_psram_); result.uid_validity = this->uid_validity_; result.total = this->exists_;
  if (page * maximum >= this->exists_) { *mailbox = std::move(result); return true; }
  const size_t end = this->exists_ - page * maximum, begin = end > maximum ? end - maximum + 1 : 1;
  result.messages.reserve(end - begin + 1);
  const bool ok = this->command_("FETCH " + std::to_string(begin) + ":" + std::to_string(end) +
      " (UID FLAGS ENVELOPE BODYSTRUCTURE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (CONTENT-DURATION)])", timeout_ms, [&](const Response &response) {
    if (!is_fetch(response.text)) return true;
    std::vector<Value> values;
    if (!parse_values(response, this->literal_, &values)) return false;
    if (values.size() < 4 || upper(values[2].text) != "FETCH") return true;
    if (values[3].kind != Value::Kind::LIST || result.messages.size() >= maximum) return false;
    const Value &fetch = values[3];
    const Value *uid = field(fetch, "UID"), *flags = field(fetch, "FLAGS"), *size = field(fetch, "RFC822.SIZE"),
                *envelope = field(fetch, "ENVELOPE"), *body = field(fetch, "BODYSTRUCTURE");
    Message item;
    if (uid == nullptr || !number(uid->text, &item.uid, true) || size == nullptr || !number(size->text, &item.size)) return false;
    item.uid_validity = this->uid_validity_;
    if (flags != nullptr) for (const auto &flag : flags->children) {
      item.seen |= upper(flag.text) == "\\SEEN"; item.deleted |= upper(flag.text) == "\\DELETED";
    }
    if (envelope != nullptr && envelope->kind == Value::Kind::LIST && envelope->children.size() >= 3) {
      item.date = value_text(envelope->children[0], response, this->literal_);
      item.subject = decode_header(value_text(envelope->children[1], response, this->literal_));
      const auto &from = envelope->children[2];
      if (from.kind == Value::Kind::LIST && !from.children.empty() && from.children[0].children.size() >= 4) {
        const auto &address = from.children[0].children;
        item.from = decode_header(value_text(address[0], response, this->literal_));
        if (item.from.empty()) item.from = value_text(address[2], response, this->literal_) + "@" + value_text(address[3], response, this->literal_);
      }
    }
    if (item.date.size() > 128 || item.subject.size() > 512 || item.from.size() > 256) return false;
    if (body != nullptr) parse_bodystructure(*body, &item.audio);
    for (size_t i = 0; i + 1 < fetch.children.size(); i += 2) {
      if (starts(upper(fetch.children[i].text), "BODY[HEADER.FIELDS ")) {
        const auto header = value_text(fetch.children[i + 1], response, this->literal_);
        item.has_duration = parse_content_duration(header, &item.duration_seconds);
      }
    }
    if (std::any_of(result.messages.begin(), result.messages.end(), [&](const Message &old) { return old.uid == item.uid; })) return false;
    result.messages.push_back(std::move(item)); return true;
  });
  if (!ok) return false;
  std::sort(result.messages.begin(), result.messages.end(), [](const Message &a, const Message &b) { return a.uid > b.uid; });
  *mailbox = std::move(result); return true;
}

bool Client::fetch_audio(uint32_t uid, const AudioPart &audio, uint32_t offset, size_t maximum,
                          size_t *received, uint32_t timeout_ms) {
  if (uid == 0 || received == nullptr || !audio.valid() || maximum == 0 || maximum > this->literal_capacity_ ||
      audio.section.size() > 64 || audio.section.find_first_not_of("0123456789.") != std::string::npos ||
      this->fetch_ == nullptr || maximum > this->fetch_capacity_) return false;
  bool found = false; size_t length = 0;
  const std::string target = "BODY[" + audio.section + "]<" + std::to_string(offset) + ">";
  const bool ok = this->command_("UID FETCH " + std::to_string(uid) + " (UID BODY.PEEK[" + audio.section + "]<" +
      std::to_string(offset) + "." + std::to_string(maximum) + ">)", timeout_ms, [&](const Response &response) {
    if (!is_fetch(response.text)) return true;
    std::vector<Value> values;
    if (!parse_values(response, this->literal_, &values)) return false;
    if (values.size() < 4 || upper(values[2].text) != "FETCH") return true;
    const Value *actual_uid = field(values[3], "UID"), *part = field(values[3], target);
    uint32_t value;
    if (actual_uid == nullptr || !number(actual_uid->text, &value, true) || value != uid || part == nullptr ||
        part->kind != Value::Kind::LITERAL || found || part->literal >= response.count) return false;
    const auto literal = response.literals[part->literal];
    if (literal.size > maximum) return false;
    // There is only one requested literal; make it available at buffer start
    // after UID and response structure have both been validated.
    std::copy_n(this->literal_ + literal.offset, literal.size, this->fetch_);
    length = literal.size; found = true; return true;
  });
  if (!ok || !found) { this->error_ = "uid_not_found"; return false; }
  *received = length; return true;
}

bool Client::store_seen(uint32_t uid, bool seen, uint32_t timeout_ms) {
  return uid != 0 && this->command_("UID STORE " + std::to_string(uid) + (seen ? " +FLAGS.SILENT (\\Seen)" : " -FLAGS.SILENT (\\Seen)"), timeout_ms);
}
bool Client::delete_uid(uint32_t uid, uint32_t timeout_ms) {
  if (uid == 0 || !this->uidplus_) { this->error_ = "uidplus_required"; return false; }
  return this->command_("UID STORE " + std::to_string(uid) + " +FLAGS.SILENT (\\Deleted)", timeout_ms) &&
      this->command_("UID EXPUNGE " + std::to_string(uid), timeout_ms);
}

bool Client::start_idle(uint32_t timeout_ms) {
  if (!this->idle_supported_ || this->idling_) return false;
  this->idle_tag_ = this->tag_();
  if (!this->send_(this->idle_tag_ + " IDLE\r\n", timeout_ms)) return false;
  const uint64_t deadline = phone_client::monotonic_ms() + timeout_ms;
  for (size_t i = 0; i < 128; ++i) {
    Response response; const auto wait = time_left(deadline);
    if (wait == 0 || !this->response_(&response, wait)) return false;
    if (starts(response.text, "+")) { this->idling_ = true; return true; }
    const auto text = upper(response.text);
    this->pending_change_ |= text.find(" EXISTS") != std::string::npos || text.find(" EXPUNGE") != std::string::npos || text.find(" FETCH ") != std::string::npos;
    if (!starts(response.text, "* ") || starts(upper(response.text), "* BYE")) return false;
  }
  return false;
}
bool Client::wait_idle(uint32_t timeout_ms, bool *changed) {
  if (!this->idling_ || changed == nullptr) return false;
  if (this->pending_change_) { this->pending_change_ = false; *changed = true; return true; }
  Response response;
  if (!this->response_(&response, timeout_ms)) return false;
  const auto text = upper(response.text);
  if (starts(text, "* BYE") || !starts(text, "* ")) return false;
  *changed = text.find(" EXISTS") != std::string::npos || text.find(" EXPUNGE") != std::string::npos || text.find(" FETCH ") != std::string::npos;
  return true;
}
bool Client::end_idle(uint32_t timeout_ms) {
  if (!this->idling_) return true;
  if (!this->send_("DONE\r\n", timeout_ms)) return false;
  this->idling_ = false;
  const uint64_t deadline = phone_client::monotonic_ms() + timeout_ms;
  for (size_t i = 0; i < 4096; ++i) {
    Response response; const auto wait = time_left(deadline);
    if (wait == 0 || !this->response_(&response, wait)) return false;
    if (starts(response.text, this->idle_tag_ + " ")) return starts(upper(response.text.substr(this->idle_tag_.size() + 1)), "OK");
    if (!starts(response.text, "* ")) return false;
  }
  return false;
}
void Client::logout(uint32_t timeout_ms) {
  if (this->idling_ && !this->end_idle(timeout_ms)) return;
  this->command_("LOGOUT", timeout_ms);
}

TransferDecoder::TransferDecoder(std::string encoding) : encoding_(upper(std::move(encoding))) {}
bool TransferDecoder::feed(const uint8_t *data, size_t length, const Sink &sink) {
  if (this->failed_) return false;
  if (this->encoding_ == "7BIT" || this->encoding_ == "8BIT" || this->encoding_ == "BINARY") {
    if (!sink(data, length)) this->failed_ = true;
    return !this->failed_;
  }
  auto hex = [](unsigned char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<unsigned char>(std::toupper(c)); return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  auto base64 = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  };
  uint8_t output[768]; size_t used = 0;
  auto emit = [&](uint8_t value) {
    output[used++] = value;
    if (used != sizeof(output)) return true;
    if (!sink(output, used)) { this->failed_ = true; return false; }
    used = 0; return true;
  };
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = data[i];
    if (this->encoding_ == "BASE64") {
      if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
      if (this->ended_) { this->failed_ = true; return false; }
      if (c != '=' && base64(c) < 0) { this->failed_ = true; return false; }
      this->pending_[this->count_++] = c;
      if (this->count_ == 4) {
        const int a = base64(this->pending_[0]), b = base64(this->pending_[1]), d = base64(this->pending_[2]), e = base64(this->pending_[3]);
        if (a < 0 || b < 0 || (this->pending_[2] == '=' && this->pending_[3] != '=')) { this->failed_ = true; return false; }
        if ((this->pending_[2] == '=' && (b & 15) != 0) ||
            (this->pending_[3] == '=' && d >= 0 && (d & 3) != 0)) { this->failed_ = true; return false; }
        if (!emit(static_cast<uint8_t>((a << 2) | (b >> 4)))) return false;
        if (this->pending_[2] != '=') {
          if (d < 0 || !emit(static_cast<uint8_t>((b << 4) | (d >> 2)))) return false;
          if (this->pending_[3] != '=' && (e < 0 || !emit(static_cast<uint8_t>((d << 6) | e)))) return false;
        }
        this->ended_ = this->pending_[2] == '=' || this->pending_[3] == '=';
        this->count_ = 0;
      }
    } else if (this->encoding_ == "QUOTED-PRINTABLE") {
      if (this->count_ == 0) {
        if (c == '=') this->pending_[this->count_++] = c;
        else if (!emit(c)) return false;
      } else if (this->count_ == 1) {
        if (c != '\r' && hex(c) < 0) { this->failed_ = true; return false; }
        this->pending_[this->count_++] = c;
      } else {
        if (this->pending_[1] == '\r') { if (c != '\n') { this->failed_ = true; return false; } }
        else {
          const int digit = hex(c);
          if (digit < 0 || !emit(static_cast<uint8_t>((hex(this->pending_[1]) << 4) | digit))) { this->failed_ = true; return false; }
        }
        this->count_ = 0;
      }
    } else { this->failed_ = true; return false; }
  }
  if (used != 0 && !sink(output, used)) this->failed_ = true;
  return !this->failed_;
}
bool TransferDecoder::finish() { return !this->failed_ && this->count_ == 0; }

std::string decode_header(std::string text) {
  if (text.find("=?") == std::string::npos) return text;
  std::string result;
  size_t position = 0;
  bool encoded_before = false;
  while (position < text.size()) {
    const size_t start = text.find("=?", position);
    if (start == std::string::npos) { result.append(text, position, std::string::npos); break; }
    const size_t charset_end = text.find('?', start + 2);
    if (charset_end == std::string::npos || charset_end + 3 >= text.size() || text[charset_end + 2] != '?') {
      result.append(text, position, std::string::npos); break;
    }
    const size_t end = text.find("?=", charset_end + 3);
    if (end == std::string::npos) { result.append(text, position, std::string::npos); break; }
    const std::string charset = upper(text.substr(start + 2, charset_end - start - 2));
    const char encoding = static_cast<char>(std::toupper(static_cast<unsigned char>(text[charset_end + 1])));
    std::string body = text.substr(charset_end + 3, end - charset_end - 3), decoded;
    if (encoding == 'Q') std::replace(body.begin(), body.end(), '_', ' ');
    TransferDecoder transfer(encoding == 'B' ? "BASE64" : encoding == 'Q' ? "QUOTED-PRINTABLE" : "unsupported");
    bool valid = body.size() <= 2048 && transfer.feed(reinterpret_cast<const uint8_t *>(body.data()), body.size(),
        [&](const uint8_t *data, size_t count) { if (decoded.size() + count > 2048) return false; decoded.append(reinterpret_cast<const char *>(data), count); return true; }) && transfer.finish();
    for (auto &c : decoded) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    if (valid && charset == "ISO-8859-1") {
      std::string utf8;
      for (const unsigned char c : decoded) {
        if (c < 128) utf8 += static_cast<char>(c);
        else { utf8 += static_cast<char>(0xc0U | (c >> 6)); utf8 += static_cast<char>(0x80U | (c & 63)); }
      }
      decoded = std::move(utf8);
    } else if (valid && charset != "UTF-8" && charset != "US-ASCII") valid = false;
    if (valid && !decoded.empty() && mailbox_wire_name(decoded).empty()) valid = false;
    const std::string prefix = text.substr(position, start - position);
    const bool only_space = std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) { return c == ' ' || c == '\t'; });
    if (!encoded_before || !only_space || !valid) result += prefix;
    if (valid) result += decoded;
    else result.append(text, start, end + 2 - start);
    encoded_before = valid;
    position = end + 2;
    if (result.size() > 4096) return text;
  }
  return result;
}

}  // namespace esphome::imap_voicemail
