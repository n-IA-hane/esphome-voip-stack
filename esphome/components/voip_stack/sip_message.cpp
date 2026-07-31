#include "sip_message.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <esp_system.h>
#include <lwip/sockets.h>

#include "esphome/core/hal.h"

namespace esphome {
namespace voip_stack {

std::string trim_copy(const std::string &s) {
  size_t begin = 0;
  while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) begin++;
  size_t end = s.size();
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
  return s.substr(begin, end - begin);
}

static bool is_sip_uri_unreserved(char ch) {
  const auto c = static_cast<unsigned char>(ch);
  return std::isalnum(c) || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

static uint8_t hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
  if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(ch - 'A' + 10);
  if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
  return 0xFF;
}

std::string sip_uri_user_encode(const std::string &raw) {
  std::string out;
  static const char *const HEX = "0123456789ABCDEF";
  for (char ch : raw) {
    if (ch == '\r' || ch == '\n') {
      continue;
    }
    if (is_sip_uri_unreserved(ch)) {
      out.push_back(ch);
    } else {
      const auto c = static_cast<unsigned char>(ch);
      out.push_back('%');
      out.push_back(HEX[(c >> 4) & 0x0F]);
      out.push_back(HEX[c & 0x0F]);
    }
  }
  if (out.empty()) out = "voip";
  return out;
}

std::string sip_uri_user_decode(const std::string &raw) {
  std::string out;
  for (size_t i = 0; i < raw.size(); i++) {
    const char ch = raw[i];
    if (ch == '\r' || ch == '\n') {
      continue;
    }
    if (ch == '%' && i + 2 < raw.size()) {
      const uint8_t hi = hex_value(raw[i + 1]);
      const uint8_t lo = hex_value(raw[i + 2]);
      if (hi != 0xFF && lo != 0xFF) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(ch);
  }
  return sip_header_token(out);
}

std::string header_value(const std::string &msg, const char *name) {
  std::string canonical = name;
  for (char &ch : canonical) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const char *compact = nullptr;
  if (canonical == "via") compact = "v";
  else if (canonical == "from") compact = "f";
  else if (canonical == "to") compact = "t";
  else if (canonical == "call-id") compact = "i";
  else if (canonical == "contact") compact = "m";
  else if (canonical == "content-type") compact = "c";
  else if (canonical == "content-length") compact = "l";
  const std::string needle = canonical + ":";
  const std::string compact_needle = compact == nullptr ? std::string() : std::string(compact) + ":";
  size_t pos = 0;
  while (pos < msg.size()) {
    const size_t end = msg.find("\r\n", pos);
    const size_t line_end = end == std::string::npos ? msg.size() : end;
    const std::string line = msg.substr(pos, line_end - pos);
    if (line.empty()) break;
    auto starts_with_header = [&line](const std::string &candidate) {
      if (candidate.empty() || line.size() < candidate.size()) return false;
      bool match = true;
      for (size_t i = 0; i < candidate.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(line[i])) !=
            std::tolower(static_cast<unsigned char>(candidate[i]))) {
          match = false;
          break;
        }
      }
      return match;
    };
    if (starts_with_header(needle)) return trim_copy(line.substr(needle.size()));
    if (starts_with_header(compact_needle)) return trim_copy(line.substr(compact_needle.size()));
    if (end == std::string::npos) break;
    pos = end + 2;
  }
  return "";
}

std::string header_values(const std::string &msg, const char *name) {
  std::string canonical = name;
  for (char &ch : canonical) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const char *compact = canonical == "via" ? "v" : nullptr;
  std::string out;
  size_t pos = 0;
  while (pos < msg.size()) {
    const size_t end = msg.find("\r\n", pos);
    const size_t line_end = end == std::string::npos ? msg.size() : end;
    const std::string line = msg.substr(pos, line_end - pos);
    if (line.empty()) break;
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = trim_copy(line.substr(0, colon));
      for (char &ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (key == canonical || (compact != nullptr && key == compact)) {
        if (!out.empty()) out += "\r\nVia: ";
        out += trim_copy(line.substr(colon + 1));
      }
    }
    if (end == std::string::npos) break;
    pos = end + 2;
  }
  return out;
}

bool parse_decimal_u32(const std::string &raw, uint32_t max_value, uint32_t *out) {
  if (out == nullptr) return false;
  const std::string value = trim_copy(raw);
  if (value.empty()) return false;
  uint32_t parsed = 0;
  for (char ch : value) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    const uint32_t digit = static_cast<uint32_t>(ch - '0');
    if (digit > max_value || parsed > (max_value - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  *out = parsed;
  return true;
}

bool time_reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

std::string message_body(const std::string &msg) {
  const size_t sep = msg.find("\r\n\r\n");
  if (sep == std::string::npos) return "";
  const size_t body_start = sep + 4;
  const std::string content_length = header_value(msg, "Content-Length");
  if (content_length.empty()) return msg.substr(body_start);
  uint32_t declared = 0;
  if (!parse_decimal_u32(content_length, MAX_SIP_BODY_BYTES, &declared) ||
      declared > msg.size() - body_start) {
    return "";
  }
  return msg.substr(body_start, declared);
}

bool sip_content_length(const std::string &msg, size_t *out) {
  if (out == nullptr) return false;
  *out = 0;
  const size_t sep = msg.find("\r\n\r\n");
  const size_t header_end = sep == std::string::npos ? msg.size() : sep;
  size_t pos = 0;
  bool seen = false;
  while (pos < header_end) {
    const size_t end = msg.find("\r\n", pos);
    const size_t line_end = end == std::string::npos ? header_end : std::min(end, header_end);
    const std::string line = msg.substr(pos, line_end - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      for (char &ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const std::string normalized = trim_copy(key);
      if (normalized == "content-length" || normalized == "l") {
        if (seen) return false;
        seen = true;
        const std::string value = trim_copy(line.substr(colon + 1));
        if (value.empty()) return false;
        size_t parsed = 0;
        for (char ch : value) {
          if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
          const size_t digit = static_cast<size_t>(ch - '0');
          if (parsed > (MAX_SIP_TCP_RX_BUFFER - digit) / 10) return false;
          parsed = parsed * 10 + digit;
        }
        *out = parsed;
      }
    }
    if (end == std::string::npos || end >= header_end) break;
    pos = end + 2;
  }
  return true;
}

std::string sip_header_token(const std::string &raw, size_t max_bytes) {
  std::string out;
  for (char ch : raw) {
    if (ch == '\r' || ch == '\n') continue;
    if (std::isalnum(static_cast<unsigned char>(ch)) ||
        ch == '_' || ch == '-' || ch == '.' || ch == ' ') {
      out.push_back(ch);
      if (out.size() >= max_bytes) break;
    }
  }
  return trim_copy(out);
}

std::string sip_quoted(const std::string &raw) {
  std::string out = "\"";
  for (char ch : raw) {
    if (ch == '\r' || ch == '\n') continue;
    if (ch == '"' || ch == '\\') out.push_back('\\');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string reason_text_from_header(const std::string &value) {
  const size_t key = value.find("text=");
  if (key == std::string::npos) return "";
  size_t begin = key + 5;
  if (begin >= value.size()) return "";
  if (value[begin] != '"') return sip_header_token(value.substr(begin));
  begin++;
  std::string out;
  bool escaped = false;
  for (size_t i = begin; i < value.size(); i++) {
    const char ch = value[i];
    if (escaped) {
      out.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') break;
    out.push_back(ch);
  }
  return sip_header_token(out);
}

std::string cseq_method(const std::string &cseq) {
  const std::string trimmed = trim_copy(cseq);
  const size_t space = trimmed.find_last_of(" \t");
  if (space == std::string::npos || space + 1 >= trimmed.size()) return "";
  return trim_copy(trimmed.substr(space + 1));
}

uint32_t cseq_number(const std::string &cseq) {
  const std::string trimmed = trim_copy(cseq);
  const size_t space = trimmed.find_first_of(" \t");
  const std::string number = space == std::string::npos ? trimmed : trimmed.substr(0, space);
  if (number.empty()) return 0;
  uint32_t parsed = 0;
  for (char ch : number) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return 0;
    const uint32_t digit = static_cast<uint32_t>(ch - '0');
    if (parsed > (UINT32_MAX - digit) / 10U) return 0;
    parsed = parsed * 10U + digit;
  }
  return parsed;
}

std::string via_branch(const std::string &via) {
  std::string lowered = via;
  for (char &ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const size_t marker = lowered.find(";branch=");
  if (marker == std::string::npos) return "";
  const size_t begin = marker + 8;
  const size_t end = via.find_first_of("; \t\r\n", begin);
  return via.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool sip_method_known_(const std::string &method) {
  return method == "INVITE" || method == "ACK" || method == "CANCEL" ||
         method == "BYE" || method == "OPTIONS" || method == "REGISTER";
}

std::string sip_failure_reason_(int status) {
  if (status == 401) return "auth_required_unsupported";
  if (status == 407) return "proxy_auth_required_unsupported";
  if (status == 486) return "busy";
  if (status == 487) return "cancelled";
  if (status == 488) return "media_incompatible";
  if (status == 603) return "declined";
  return "sip_" + std::to_string(status);
}

std::string tag_from_header(const std::string &value) {
  // With name-addr syntax, header parameters start after '>'. Without angle
  // brackets they start at the first semicolon. Parsing parameter keys avoids
  // treating display names or URI users containing "tag=" as dialog tags.
  const size_t right_angle = value.find('>');
  size_t pos = value.find(';', right_angle == std::string::npos ? 0 : right_angle + 1);
  while (pos != std::string::npos) {
    const size_t next = value.find(';', pos + 1);
    const std::string parameter = trim_copy(value.substr(
        pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1));
    const size_t equals = parameter.find('=');
    if (equals != std::string::npos) {
      std::string key = trim_copy(parameter.substr(0, equals));
      for (char &ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (key == "tag") {
        std::string tag = trim_copy(parameter.substr(equals + 1));
        const size_t whitespace = tag.find_first_of(" \t\r\n");
        if (whitespace != std::string::npos) tag.resize(whitespace);
        return tag;
      }
    }
    pos = next;
  }
  return "";
}

std::string strip_angle_uri(const std::string &value) {
  std::string out = trim_copy(value);
  const size_t left = out.find('<');
  const size_t right = left == std::string::npos ? std::string::npos : out.find('>', left + 1);
  if (left != std::string::npos && right != std::string::npos && right > left + 1) {
    out = trim_copy(out.substr(left + 1, right - left - 1));
  }
  std::string lowered = out;
  for (char &ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const size_t tag = lowered.find(";tag=");
  if (tag != std::string::npos) {
    out.resize(tag);
    lowered.resize(tag);
  }
  if (lowered.rfind("sip:", 0) != 0 || out.find_first_of("\r\n") != std::string::npos) return "";
  return out;
}

bool sip_uri_ipv4_target(const std::string &value, uint32_t *ip_v4, uint16_t *port) {
  if (ip_v4 == nullptr || port == nullptr) return false;
  std::string uri = strip_angle_uri(value);
  if (uri.empty()) return false;
  std::string authority = uri.substr(4);
  const size_t params = authority.find(';');
  if (params != std::string::npos) authority.resize(params);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  authority = trim_copy(authority);
  if (authority.empty()) return false;

  uint16_t parsed_port = 5060;
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    const std::string port_text = authority.substr(colon + 1);
    if (port_text.empty()) return false;
    uint32_t port_value = 0;
    if (!parse_decimal_u32(port_text, UINT16_MAX, &port_value)) return false;
    if (port_value == 0) return false;
    parsed_port = static_cast<uint16_t>(port_value);
    authority.resize(colon);
  }

  struct in_addr address{};
  if (authority.empty() || inet_aton(authority.c_str(), &address) == 0 || address.s_addr == 0) return false;
  *ip_v4 = ntohl(address.s_addr);
  *port = parsed_port;
  return true;
}

std::string sip_user_from_header(const std::string &value) {
  std::string uri;
  const size_t left = value.find('<');
  const size_t right = left == std::string::npos ? std::string::npos : value.find('>', left + 1);
  if (left != std::string::npos && right != std::string::npos && right > left + 1) {
    uri = value.substr(left + 1, right - left - 1);
  } else {
    uri = trim_copy(value);
    const size_t semicolon = uri.find(';');
    if (semicolon != std::string::npos) uri.resize(semicolon);
  }
  uri = trim_copy(uri);
  if (uri.size() >= 4 && std::tolower(static_cast<unsigned char>(uri[0])) == 's' &&
      std::tolower(static_cast<unsigned char>(uri[1])) == 'i' &&
      std::tolower(static_cast<unsigned char>(uri[2])) == 'p' && uri[3] == ':') {
    uri = uri.substr(4);
  }
  const size_t at = uri.find('@');
  if (at == std::string::npos || at == 0) return "";
  return sip_uri_user_decode(uri.substr(0, at));
}

std::string response_via_with_rport(const std::string &via, uint32_t source_ip, uint16_t source_port) {
  const size_t remaining_vias = via.find("\r\nVia: ");
  const std::string top_via = remaining_vias == std::string::npos ? via : via.substr(0, remaining_vias);
  const std::string tail = remaining_vias == std::string::npos ? "" : via.substr(remaining_vias);
  std::string lowered = top_via;
  for (char &ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (lowered.find("rport") == std::string::npos) return top_via + tail;
  struct in_addr a{};
  a.s_addr = htonl(source_ip);
  char ip_text[16];
  inet_ntoa_r(a, ip_text, sizeof(ip_text));
  const size_t first_semicolon = top_via.find(';');
  if (first_semicolon == std::string::npos) {
    return top_via + ";received=" + std::string(ip_text) + ";rport=" + std::to_string(source_port) + tail;
  }
  std::string out = top_via.substr(0, first_semicolon);
  size_t pos = first_semicolon + 1;
  while (pos <= top_via.size()) {
    const size_t end = top_via.find(';', pos);
    const size_t part_end = end == std::string::npos ? top_via.size() : end;
    const std::string part = trim_copy(top_via.substr(pos, part_end - pos));
    const size_t eq = part.find('=');
    std::string key = trim_copy(eq == std::string::npos ? part : part.substr(0, eq));
    for (char &ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (!part.empty() && key != "rport" && key != "received") {
      out += ";" + part;
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  out += ";received=" + std::string(ip_text) + ";rport=" + std::to_string(source_port);
  return out + tail;
}

std::string make_token(const char *prefix) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%s%08x%08x", prefix,
           static_cast<unsigned>(esp_random()),
           static_cast<unsigned>(millis()));
  return buf;
}

bool parse_rtpmap_format(const std::string &line, AudioFormat *fmt, uint8_t *payload_type) {
  // a=rtpmap:96 L16/16000/1
  const size_t colon = line.find(':');
  const size_t space = line.find(' ', colon == std::string::npos ? 0 : colon + 1);
  if (colon == std::string::npos || space == std::string::npos) return false;
  uint32_t pt = 0;
  if (!parse_decimal_u32(line.substr(colon + 1, space - colon - 1), 127, &pt)) return false;
  const std::string spec = trim_copy(line.substr(space + 1));
  const size_t slash1 = spec.find('/');
  const size_t slash2 = slash1 == std::string::npos ? std::string::npos : spec.find('/', slash1 + 1);
  if (slash1 == std::string::npos) return false;
  const std::string enc = spec.substr(0, slash1);
  uint32_t rate = 0;
  uint32_t channels = 1;
  if (!parse_decimal_u32(spec.substr(slash1 + 1, slash2 - slash1 - 1), UINT32_MAX, &rate) ||
      (slash2 != std::string::npos && !parse_decimal_u32(spec.substr(slash2 + 1), UINT8_MAX, &channels))) {
    return false;
  }
  AudioFormat candidate;
  candidate.sample_rate = rate;
  candidate.channels = static_cast<uint8_t>(channels);
  candidate.frame_ms = 20;
  if (enc == "L16" || enc == "l16") {
    candidate.pcm_format = PcmFormat::S16LE;
  } else if (enc == "L24" || enc == "l24") {
    candidate.pcm_format = PcmFormat::S24LE;
  } else {
    return false;
  }
  if (!candidate.is_valid()) return false;
  *fmt = candidate;
  *payload_type = static_cast<uint8_t>(pt);
  return true;
}

bool parse_audio_media_line(const std::string &line, uint16_t *port, bool payload_types[128]) {
  if (port == nullptr || payload_types == nullptr || line.rfind("m=audio ", 0) != 0) return false;
  const std::string media = trim_copy(line.substr(8));
  const size_t port_end = media.find_first_of(" \t");
  if (port_end == std::string::npos) return false;
  size_t protocol_start = media.find_first_not_of(" \t", port_end);
  if (protocol_start == std::string::npos) return false;
  const size_t protocol_end = media.find_first_of(" \t", protocol_start);
  if (protocol_end == std::string::npos || media.substr(protocol_start, protocol_end - protocol_start) != "RTP/AVP") {
    return false;
  }
  uint32_t parsed_port = 0;
  if (!parse_decimal_u32(media.substr(0, port_end), UINT16_MAX, &parsed_port) || parsed_port == 0) return false;

  bool any_payload = false;
  size_t pos = media.find_first_not_of(" \t", protocol_end);
  while (pos != std::string::npos) {
    const size_t end = media.find_first_of(" \t", pos);
    uint32_t payload_type = 0;
    if (!parse_decimal_u32(media.substr(pos, end == std::string::npos ? std::string::npos : end - pos),
                           127, &payload_type)) {
      return false;
    }
    payload_types[payload_type] = true;
    any_payload = true;
    if (end == std::string::npos) break;
    pos = media.find_first_not_of(" \t", end);
  }
  if (!any_payload) return false;
  *port = static_cast<uint16_t>(parsed_port);
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
bool parse_video_media_line(const std::string &line, uint16_t *port,
                            bool payload_types[128],
                            uint8_t *payload_order,
                            size_t *payload_count) {
  if (port == nullptr || payload_types == nullptr ||
      line.rfind("m=video ", 0) != 0) {
    return false;
  }
  const std::string media = trim_copy(line.substr(8));
  const size_t port_end = media.find_first_of(" \t");
  if (port_end == std::string::npos) return false;
  const size_t protocol_start = media.find_first_not_of(" \t", port_end);
  if (protocol_start == std::string::npos) return false;
  const size_t protocol_end = media.find_first_of(" \t", protocol_start);
  if (protocol_end == std::string::npos ||
      media.substr(protocol_start, protocol_end - protocol_start) !=
          "RTP/AVP") {
    return false;
  }
  uint32_t parsed_port = 0;
  if (!parse_decimal_u32(media.substr(0, port_end), UINT16_MAX,
                         &parsed_port)) {
    return false;
  }
  bool any = false;
  size_t count = 0;
  size_t pos = media.find_first_not_of(" \t", protocol_end);
  while (pos != std::string::npos) {
    const size_t end = media.find_first_of(" \t", pos);
    uint32_t payload_type = 0;
    if (!parse_decimal_u32(
            media.substr(pos, end == std::string::npos
                                  ? std::string::npos
                                  : end - pos),
            127, &payload_type)) {
      return false;
    }
    if (count >= 128) return false;
    payload_types[payload_type] = true;
    if (payload_order != nullptr)
      payload_order[count] = static_cast<uint8_t>(payload_type);
    count++;
    any = true;
    if (end == std::string::npos) break;
    pos = media.find_first_not_of(" \t", end);
  }
  if (!any) return false;
  if (payload_count != nullptr) *payload_count = count;
  *port = static_cast<uint16_t>(parsed_port);
  return true;
}

bool parse_video_rtpmap(const std::string &line, uint8_t *payload_type,
                        std::string *encoding) {
  if (payload_type == nullptr || encoding == nullptr ||
      line.rfind("a=rtpmap:", 0) != 0)
    return false;
  const size_t space = line.find(' ', 9);
  if (space == std::string::npos) return false;
  uint32_t parsed_pt = 0;
  if (!parse_decimal_u32(line.substr(9, space - 9), 127, &parsed_pt))
    return false;
  *encoding = trim_copy(line.substr(space + 1));
  for (char &ch : *encoding)
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  *payload_type = static_cast<uint8_t>(parsed_pt);
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
bool parse_video_rtcp_feedback(const std::string &line, bool payloads[128],
                               bool *pli, bool *fir) {
  if (payloads == nullptr || pli == nullptr || fir == nullptr ||
      line.rfind("a=rtcp-fb:", 0) != 0) {
    return false;
  }
  const size_t space = line.find(' ', 10);
  if (space == std::string::npos) return false;
  const std::string selector = trim_copy(line.substr(10, space - 10));
  uint32_t parsed_pt = 0;
  const bool wildcard = selector == "*";
  if (!wildcard &&
      !parse_decimal_u32(selector, 127, &parsed_pt)) {
    return false;
  }
  std::string feedback = trim_copy(line.substr(space + 1));
  for (char &ch : feedback) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  const bool is_pli = feedback == "nack pli";
  const bool is_fir = feedback == "ccm fir";
  if (!is_pli && !is_fir) return false;
  for (size_t pt = 0; pt < 128; pt++) {
    if ((wildcard || pt == parsed_pt) && payloads[pt]) {
      if (is_pli) pli[pt] = true;
      if (is_fir) fir[pt] = true;
    }
  }
  return true;
}
#endif

bool parse_rtcp_attribute(const std::string &line, uint16_t *port,
                          uint32_t *ip_v4, bool *has_address) {
  if (port == nullptr || ip_v4 == nullptr || has_address == nullptr ||
      line.rfind("a=rtcp:", 0) != 0) {
    return false;
  }
  const std::string value = trim_copy(line.substr(7));
  const size_t port_end = value.find_first_of(" \t");
  uint32_t parsed_port = 0;
  if (!parse_decimal_u32(
          value.substr(0, port_end == std::string::npos
                              ? std::string::npos
                              : port_end),
          UINT16_MAX, &parsed_port) ||
      parsed_port == 0) {
    return false;
  }
  *port = static_cast<uint16_t>(parsed_port);
  *has_address = false;
  if (port_end == std::string::npos) return true;
  const size_t address_spec = value.find_first_not_of(" \t", port_end);
  if (address_spec == std::string::npos) return true;
  static constexpr char PREFIX[] = "IN IP4 ";
  if (value.compare(address_spec, sizeof(PREFIX) - 1, PREFIX) != 0)
    return false;
  struct in_addr address{};
  if (inet_aton(value.substr(address_spec + sizeof(PREFIX) - 1).c_str(),
                &address) == 0 ||
      address.s_addr == 0) {
    return false;
  }
  *ip_v4 = ntohl(address.s_addr);
  *has_address = true;
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
std::string fmtp_parameter(const std::string &line, const char *name,
                           uint8_t *payload_type) {
  if (payload_type == nullptr || line.rfind("a=fmtp:", 0) != 0) return "";
  const size_t space = line.find(' ', 7);
  if (space == std::string::npos) return "";
  uint32_t parsed_pt = 0;
  if (!parse_decimal_u32(line.substr(7, space - 7), 127, &parsed_pt))
    return "";
  *payload_type = static_cast<uint8_t>(parsed_pt);
  std::string params = line.substr(space + 1);
  std::string wanted = name;
  for (char &ch : wanted)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  size_t pos = 0;
  while (pos < params.size()) {
    const size_t end = params.find(';', pos);
    const std::string item =
        trim_copy(params.substr(pos, end == std::string::npos
                                        ? std::string::npos
                                        : end - pos));
    const size_t equals = item.find('=');
    if (equals != std::string::npos) {
      std::string key = trim_copy(item.substr(0, equals));
      for (char &ch : key)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (key == wanted) return trim_copy(item.substr(equals + 1));
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return "";
}
#endif
#endif

size_t pcm_to_rtp_payload(const uint8_t *pcm, size_t bytes, const AudioFormat &format,
                          uint8_t *dst, size_t dst_cap) {
  if (pcm == nullptr || dst == nullptr || bytes == 0) return 0;
  if (format.pcm_format == PcmFormat::S16LE) {
    if ((bytes % 2) != 0 || bytes > dst_cap) return 0;
    for (size_t i = 0; i < bytes; i += 2) {
      dst[i] = pcm[i + 1];
      dst[i + 1] = pcm[i];
    }
    return bytes;
  }
  if (format.pcm_format == PcmFormat::S24LE) {
    if ((bytes % 3) != 0 || bytes > dst_cap) return 0;
    for (size_t i = 0; i < bytes; i += 3) {
      dst[i] = pcm[i + 2];
      dst[i + 1] = pcm[i + 1];
      dst[i + 2] = pcm[i];
    }
    return bytes;
  }
  if (format.pcm_format == PcmFormat::S24LE_IN_S32) {
    if ((bytes % 4) != 0 || bytes / 4 * 3 > dst_cap) return 0;
    size_t out = 0;
    for (size_t i = 0; i < bytes; i += 4) {
      dst[out++] = pcm[i + 2];
      dst[out++] = pcm[i + 1];
      dst[out++] = pcm[i];
    }
    return out;
  }
  return 0;
}

size_t rtp_payload_to_pcm(const uint8_t *payload, size_t payload_len, const AudioFormat &format,
                          uint8_t *pcm, size_t pcm_cap) {
  if (payload == nullptr || pcm == nullptr || payload_len == 0) return 0;
  if (format.pcm_format == PcmFormat::S16LE) {
    if ((payload_len % 2) != 0 || payload_len > pcm_cap) return 0;
    for (size_t i = 0; i < payload_len; i += 2) {
      pcm[i] = payload[i + 1];
      pcm[i + 1] = payload[i];
    }
    return payload_len;
  }
  if (format.pcm_format == PcmFormat::S24LE) {
    if ((payload_len % 3) != 0 || payload_len > pcm_cap) return 0;
    for (size_t i = 0; i < payload_len; i += 3) {
      pcm[i] = payload[i + 2];
      pcm[i + 1] = payload[i + 1];
      pcm[i + 2] = payload[i];
    }
    return payload_len;
  }
  if (format.pcm_format == PcmFormat::S24LE_IN_S32) {
    if ((payload_len % 3) != 0 || payload_len / 3 * 4 > pcm_cap) return 0;
    size_t out = 0;
    for (size_t i = 0; i < payload_len; i += 3) {
      pcm[out++] = payload[i + 2];
      pcm[out++] = payload[i + 1];
      pcm[out++] = payload[i];
      pcm[out++] = payload[i] & 0x80 ? 0xFF : 0x00;
    }
    return out;
  }
  return 0;
}

}  // namespace voip_stack
}  // namespace esphome

#endif
