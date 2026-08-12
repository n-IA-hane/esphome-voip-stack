#include "sip_transport.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <esp_system.h>

#include "audio_core_task_utils.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "net_utils.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.sip";

namespace {

static constexpr size_t MAX_SIP_BODY_BYTES = 4096;
static constexpr size_t MAX_SIP_TCP_RX_BUFFER = 8192;
static constexpr uint32_t SIP_T1_MS = 500;
static constexpr uint32_t SIP_T2_MS = 4000;
static constexpr uint32_t SIP_TRANSACTION_TIMEOUT_MS = 64 * SIP_T1_MS;

std::string sip_header_token(const std::string &raw, size_t max_bytes = VOIP_STACK_MAX_REASON_LEN);

std::string trim_copy(const std::string &s) {
  size_t begin = 0;
  while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) begin++;
  size_t end = s.size();
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
  return s.substr(begin, end - begin);
}

bool is_sip_uri_unreserved(char ch) {
  const auto c = static_cast<unsigned char>(ch);
  return std::isalnum(c) || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

uint8_t hex_value(char ch) {
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

}  // namespace

SipTransport::SipTransport(uint16_t sip_port, uint16_t rtp_port, size_t udp_max_payload,
                           const std::string &remote_host,
                           bool task_stacks_in_psram)
    : sip_port_(sip_port), rtp_port_(rtp_port), udp_max_payload_(udp_max_payload),
      task_stacks_in_psram_(task_stacks_in_psram) {
  audio_format_list_default(&this->offer_tx_formats_);
  audio_format_list_default(&this->offer_rx_formats_);
  this->rtp_ssrc_ = esp_random();
  this->parse_remote_(remote_host);
}

SipTransport::~SipTransport() { this->stop(); }

const char *SipTransport::sip_event_name_(SipEvent event) {
  switch (event) {
    case SipEvent::INVITE: return "INVITE";
    case SipEvent::ACK: return "ACK";
    case SipEvent::CANCEL: return "CANCEL";
    case SipEvent::BYE: return "BYE";
    case SipEvent::OPTIONS: return "OPTIONS";
    case SipEvent::RESPONSE: return "SIP_RESPONSE";
    case SipEvent::NONE:
    default: return "";
  }
}

SipTransport::SipEvent SipTransport::sip_event_from_method_(const std::string &method) {
  if (method == "INVITE") return SipEvent::INVITE;
  if (method == "ACK") return SipEvent::ACK;
  if (method == "CANCEL") return SipEvent::CANCEL;
  if (method == "BYE") return SipEvent::BYE;
  if (method == "OPTIONS") return SipEvent::OPTIONS;
  return SipEvent::NONE;
}

void SipTransport::mark_sip_event_(SipEvent event, uint16_t status) {
  this->last_sip_event_.store(static_cast<uint8_t>(event), std::memory_order_release);
  if (status != 0) {
    this->last_sip_status_code_.store(status, std::memory_order_release);
  }
}

SipTransportSnapshot SipTransport::snapshot() const {
  SipTransportSnapshot out;
  out.running = this->running_.load(std::memory_order_acquire);
  out.rtp_running = this->rtp_running_.load(std::memory_order_acquire);
  out.call_active = this->media_active_.load(std::memory_order_acquire);
  out.pending_invite = this->outgoing_invite_pending_.load(std::memory_order_acquire);
  out.sip_tcp = this->remote_sip_tcp_.load(std::memory_order_acquire);
  out.remote_sip_port = this->remote_sip_port_.load(std::memory_order_acquire);
  out.remote_rtp_port = this->remote_rtp_port_.load(std::memory_order_acquire);
  this->get_media_config_(&out.selected_tx_format, &out.selected_rx_format, nullptr, nullptr);
  out.rtp_tx_packets = this->rtp_tx_packets_.load(std::memory_order_acquire);
  out.rtp_rx_packets = this->rtp_rx_packets_.load(std::memory_order_acquire);
  out.rtp_tx_bytes = this->rtp_tx_bytes_.load(std::memory_order_acquire);
  out.rtp_rx_bytes = this->rtp_rx_bytes_.load(std::memory_order_acquire);
  out.last_sip_status_code = this->last_sip_status_code_.load(std::memory_order_acquire);
  out.last_sip_event = SipTransport::sip_event_name_(
      static_cast<SipEvent>(this->last_sip_event_.load(std::memory_order_acquire)));
  return out;
}

void SipTransport::set_media_config_(const AudioFormat &tx, const AudioFormat &rx,
                                     uint8_t tx_payload_type, uint8_t rx_payload_type) {
  portENTER_CRITICAL(&this->media_config_lock_);
  this->selected_tx_format_ = tx;
  this->selected_rx_format_ = rx;
  this->rtp_tx_payload_type_ = tx_payload_type;
  this->rtp_rx_payload_type_ = rx_payload_type;
  portEXIT_CRITICAL(&this->media_config_lock_);
}

void SipTransport::get_media_config_(AudioFormat *tx, AudioFormat *rx,
                                     uint8_t *tx_payload_type, uint8_t *rx_payload_type) const {
  portENTER_CRITICAL(&this->media_config_lock_);
  if (tx != nullptr) *tx = this->selected_tx_format_;
  if (rx != nullptr) *rx = this->selected_rx_format_;
  if (tx_payload_type != nullptr) *tx_payload_type = this->rtp_tx_payload_type_;
  if (rx_payload_type != nullptr) *rx_payload_type = this->rtp_rx_payload_type_;
  portEXIT_CRITICAL(&this->media_config_lock_);
}

void SipTransport::set_audio_formats(const AudioFormatList &tx, const AudioFormatList &rx) {
  this->offer_tx_formats_ = tx;
  this->offer_rx_formats_ = rx;
  if (this->offer_tx_formats_.count == 0) audio_format_list_default(&this->offer_tx_formats_);
  if (this->offer_rx_formats_.count == 0) audio_format_list_default(&this->offer_rx_formats_);
  ESP_LOGI(TAG, "SIP media capabilities: tx=%u rx=%u",
           (unsigned) this->offer_tx_formats_.count,
           (unsigned) this->offer_rx_formats_.count);
}

bool SipTransport::parse_remote_(const std::string &host) {
  if (host.empty()) {
    this->remote_ip_v4_.store(0, std::memory_order_release);
    this->remote_rtp_ip_v4_.store(0, std::memory_order_release);
    return false;
  }
  struct in_addr a{};
  if (inet_aton(host.c_str(), &a) == 0 || a.s_addr == 0) {
    this->remote_ip_v4_.store(0, std::memory_order_release);
    this->remote_rtp_ip_v4_.store(0, std::memory_order_release);
    return false;
  }
  const uint32_t ip_v4 = ntohl(a.s_addr);
  this->remote_ip_v4_.store(ip_v4, std::memory_order_release);
  this->remote_rtp_ip_v4_.store(ip_v4, std::memory_order_release);
  return true;
}

bool SipTransport::bind_udp_(int *fd, uint16_t port, const char *label) {
  *fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (*fd < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to create %s socket: %s (%d: %s)",
             label, socket_errno_name(err), err, socket_errno_text(err));
    return false;
  }
  int flags = fcntl(*fd, F_GETFL, 0);
  fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
  if (strcmp(label, "RTP") == 0) {
    const int rx_buffer = kRtpSocketRxBufferBytes;
    setsockopt(*fd, SOL_SOCKET, SO_RCVBUF, &rx_buffer, sizeof(rx_buffer));
    const int tos = 0xB8;
    setsockopt(*fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  }
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(*fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s bind on UDP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  return true;
}

bool SipTransport::bind_tcp_(int *fd, uint16_t port, const char *label) {
  *fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*fd < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "Failed to create %s TCP socket: %s (%d: %s)",
             label, socket_errno_name(err), err, socket_errno_text(err));
    return false;
  }
  int opt = 1;
  setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int flags = fcntl(*fd, F_GETFL, 0);
  fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(*fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s bind on TCP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  if (listen(*fd, 2) < 0) {
    const int err = errno;
    ESP_LOGE(TAG, "%s listen on TCP/%u failed: %s (%d: %s)",
             label, (unsigned) port, socket_errno_name(err), err, socket_errno_text(err));
    close(*fd);
    *fd = -1;
    return false;
  }
  return true;
}

bool SipTransport::start() {
  if (this->running_.load(std::memory_order_acquire)) return true;
  if (this->sip_task_handle_ != nullptr || this->rtp_task_handle_ != nullptr) {
    ESP_LOGE(TAG, "Cannot start SIP transport while a previous task is still owned");
    return false;
  }
  if (this->sip_task_done_ == nullptr) {
    this->sip_task_done_ = xSemaphoreCreateBinaryStatic(&this->sip_task_done_storage_);
  }
  if (this->sip_task_done_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create SIP task completion signal");
    return false;
  }
  xSemaphoreTake(this->sip_task_done_, 0);
  if (!this->bind_udp_(&this->sip_socket_, this->sip_port_, "SIP")) return false;
  if (!this->bind_tcp_(&this->sip_tcp_listener_socket_, this->sip_port_, "SIP")) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    return false;
  }
  this->running_.store(true, std::memory_order_release);
  if (!voip_audio_core::start_pinned_task(SipTransport::sip_task_trampoline_, "voip_sip",
                                          kSipTaskStackBytes, this, kSipTaskPriority, 1,
                                          this->task_stacks_in_psram_, TAG,
                                          &this->sip_task_handle_, &this->sip_task_tcb_,
                                          &this->sip_task_stack_)) {
    this->running_.store(false, std::memory_order_release);
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
    return false;
  }
  if (this->rtp_task_done_ == nullptr) {
    this->rtp_task_done_ = xSemaphoreCreateBinaryStatic(&this->rtp_task_done_storage_);
  }
  if (this->rtp_task_done_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create RTP task completion signal");
    this->stop();
    return false;
  }
  xSemaphoreTake(this->rtp_task_done_, 0);
  this->rtp_task_quiesced_.store(true, std::memory_order_release);
  this->rtp_task_terminate_.store(false, std::memory_order_release);
  if (!voip_audio_core::start_pinned_task(SipTransport::rtp_task_trampoline_, "voip_rtp",
                                          kRtpTaskStackBytes, this, kRtpTaskPriority, 1,
                                          this->task_stacks_in_psram_, TAG,
                                          &this->rtp_task_handle_, &this->rtp_task_tcb_,
                                          &this->rtp_task_stack_)) {
    this->stop();
    return false;
  }
  ESP_LOGI(TAG, "SIP listening on UDP+TCP/%u, RTP base UDP/%u", (unsigned) this->sip_port_, (unsigned) this->rtp_port_);
  this->emit_connection_change_(true);
  return true;
}

void SipTransport::request_tcp_client_close_() {
  LockGuard send_lock(this->tcp_send_mutex_);
  const int socket = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
  this->tcp_connect_requested_.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  }
  this->sip_tcp_client_close_requested_.store(true, std::memory_order_release);
  if (socket >= 0) shutdown(socket, SHUT_RDWR);
  this->wake_sip_task_();
}

void SipTransport::close_tcp_client_from_sip_task_() {
  LockGuard send_lock(this->tcp_send_mutex_);
  const int socket = this->sip_tcp_client_socket_.exchange(-1, std::memory_order_acq_rel);
  this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
  this->sip_tcp_client_ip_v4_.store(0, std::memory_order_release);
  if (socket >= 0) close(socket);
  this->sip_tcp_rx_buffer_.clear();
}

void SipTransport::handle_tcp_peer_loss_() {
  bool dialog_lost = false;
  {
    LockGuard lock(this->dialog_mutex_);
    dialog_lost = !this->call_id_.empty() ||
                  this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
                  this->media_active_.load(std::memory_order_acquire);
    this->close_tcp_client_from_sip_task_();
    this->remote_sip_tcp_.store(false, std::memory_order_release);
    if (dialog_lost) this->reset_dialog_();
  }
  if (dialog_lost) this->emit_connection_change_(false);
}

void SipTransport::wake_sip_task_() {
  if (this->sip_task_handle_ != nullptr) {
    xTaskNotifyGive(this->sip_task_handle_);
  }
  const int socket = this->sip_socket_;
  if (socket < 0) return;
  struct sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  self.sin_port = htons(this->sip_port_);
  sendto(socket, "", 0, 0, reinterpret_cast<struct sockaddr *>(&self), sizeof(self));
}

void SipTransport::wake_rtp_task_() {
  const int socket = this->rtp_socket_;
  if (socket >= 0) {
    // The active task is blocked in select(), so wake it through the socket.
    // Also giving a task notification here would survive the inner media loop
    // and produce a second, false quiesced completion before the task parks.
    struct sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    self.sin_port = htons(this->rtp_port_);
    sendto(socket, "", 0, 0, reinterpret_cast<struct sockaddr *>(&self), sizeof(self));
  } else if (this->rtp_task_handle_ != nullptr) {
    // No socket means the task is parked in ulTaskNotifyTake().
    xTaskNotifyGive(this->rtp_task_handle_);
  }
}

void SipTransport::stop() {
  this->stop_audio_path();
  if (this->rtp_task_handle_ != nullptr) {
    if (!this->rtp_task_quiesced_.load(std::memory_order_acquire)) {
      ESP_LOGE(TAG, "RTP task is not quiesced; retaining its task resources");
    } else {
      xSemaphoreTake(this->rtp_task_done_, 0);
      this->rtp_task_terminate_.store(true, std::memory_order_release);
      xTaskNotifyGive(this->rtp_task_handle_);
      if (xSemaphoreTake(this->rtp_task_done_, pdMS_TO_TICKS(1000)) == pdTRUE) {
        voip_audio_core::cleanup_pinned_task(&this->rtp_task_handle_, &this->rtp_task_stack_, kRtpTaskStackBytes);
      } else {
        ESP_LOGE(TAG, "RTP task did not terminate cleanly; retaining its task resources");
      }
    }
  }
  if (!this->running_.exchange(false, std::memory_order_acq_rel)) return;
  // The SIP task can be blocked in select/recv. Wake it and wait for its
  // self-termination before closing its sockets or releasing its stack;
  // deleting it from another core can strand lwIP locks or free live memory.
  this->tcp_connect_requested_.store(false, std::memory_order_release);
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  }
  this->sip_tcp_client_close_requested_.store(true, std::memory_order_release);
  this->wake_sip_task_();
  if (this->sip_task_done_ != nullptr && xSemaphoreTake(this->sip_task_done_, pdMS_TO_TICKS(1000)) == pdTRUE) {
    voip_audio_core::cleanup_pinned_task(&this->sip_task_handle_, &this->sip_task_stack_, kSipTaskStackBytes);
    if (this->sip_socket_ >= 0) {
      close(this->sip_socket_);
      this->sip_socket_ = -1;
    }
    if (this->sip_tcp_listener_socket_ >= 0) {
      close(this->sip_tcp_listener_socket_);
      this->sip_tcp_listener_socket_ = -1;
    }
  } else {
    ESP_LOGE(TAG, "SIP task did not stop cleanly; retaining its task and socket resources");
  }
  this->emit_connection_change_(false);
}

bool SipTransport::is_connected() const {
  return this->running_.load(std::memory_order_acquire);
}

void SipTransport::disconnect() {
  this->stop_audio_path();
  LockGuard lock(this->dialog_mutex_);
  this->reset_dialog_();
}

bool SipTransport::start_audio_path() {
  if (this->rtp_running_.load(std::memory_order_acquire)) return true;
  if (this->rtp_task_handle_ == nullptr || this->rtp_task_terminate_.load(std::memory_order_acquire) ||
      !this->rtp_task_quiesced_.load(std::memory_order_acquire)) {
    ESP_LOGE(TAG, "RTP task is unavailable for a new media session");
    return false;
  }
  this->reset_rtp_latch_();
  this->rtp_sequence_.store(static_cast<uint16_t>(esp_random()), std::memory_order_release);
  this->rtp_timestamp_.store(esp_random(), std::memory_order_release);
  this->rtp_ssrc_ = esp_random();
  if (!this->bind_udp_(&this->rtp_socket_, this->rtp_port_, "RTP")) return false;
  xSemaphoreTake(this->rtp_task_done_, 0);
  this->rtp_task_quiesced_.store(false, std::memory_order_release);
  this->rtp_running_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->rtp_task_handle_);
  return true;
}

void SipTransport::stop_audio_path() {
  this->close_media_session_();
  if (!this->rtp_running_.exchange(false, std::memory_order_acq_rel)) return;
  this->wake_rtp_task_();
  if (this->rtp_task_done_ != nullptr && xSemaphoreTake(this->rtp_task_done_, pdMS_TO_TICKS(1000)) == pdTRUE) {
    LockGuard socket_lock(this->rtp_socket_mutex_);
    if (this->rtp_socket_ >= 0) {
      close(this->rtp_socket_);
      this->rtp_socket_ = -1;
    }
  } else {
    ESP_LOGE(TAG, "RTP task did not quiesce cleanly; retaining its socket and task resources");
  }
}

bool SipTransport::originate(const std::string &host, uint16_t port) {
  if (!this->parse_remote_(host)) return false;
  const uint16_t sip_port = port ? port : 5060;
  this->remote_sip_port_.store(sip_port, std::memory_order_release);
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    ESP_LOGI(TAG, "SIP UDP originate target set to %s:%u", host.c_str(), (unsigned) sip_port);
    return true;
  }

  this->request_tcp_client_close_();
  const uint32_t ip_v4 = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (ip_v4 == 0) return false;
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  }
  this->tcp_connect_ip_v4_.store(ip_v4, std::memory_order_release);
  this->tcp_connect_port_.store(sip_port, std::memory_order_release);
  this->tcp_connect_requested_.store(true, std::memory_order_release);
  this->wake_sip_task_();
  return true;
}

void SipTransport::set_remote(const std::string &ip, uint16_t port, uint16_t rtp_port) {
  this->parse_remote_(ip);
  if (port) this->remote_sip_port_.store(port, std::memory_order_release);
  if (rtp_port) this->remote_rtp_port_.store(rtp_port, std::memory_order_release);
}

void SipTransport::set_sip_signaling_transport(bool tcp) {
  const bool was_tcp = this->remote_sip_tcp_.exchange(tcp, std::memory_order_acq_rel);
  if (!tcp && was_tcp) this->request_tcp_client_close_();
}

void SipTransport::clear_invite_transaction_() {
  this->pending_invite_.clear();
}

void SipTransport::clear_bye_transaction_() {
  this->pending_bye_.clear();
}

void SipTransport::clear_udp_transactions_() {
  this->pending_invite_.clear();
  this->pending_cancel_.clear();
  this->pending_bye_.clear();
}

void SipTransport::reset_rtp_latch_() {
  this->latched_rtp_ip_v4_.store(0, std::memory_order_release);
  this->latched_rtp_port_.store(0, std::memory_order_release);
  this->latched_rtp_ssrc_.store(0, std::memory_order_release);
  this->rtp_ssrc_latched_.store(false, std::memory_order_release);
}

void SipTransport::open_media_session_() {
  this->media_active_.store(true, std::memory_order_release);
}

void SipTransport::close_media_session_() {
  this->media_active_.store(false, std::memory_order_release);
}

void SipTransport::reset_dialog_() {
  this->stop_audio_path();
  {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  }
  this->call_id_.clear();
  this->local_tag_.clear();
  this->remote_tag_.clear();
  this->branch_.clear();
  this->local_uri_.clear();
  this->remote_uri_.clear();
  this->remote_target_uri_.clear();
  this->last_invite_via_.clear();
  this->last_invite_from_.clear();
  this->last_invite_to_.clear();
  this->last_invite_cseq_.clear();
  this->last_invite_response_.clear();
  this->last_invite_cseq_number_ = 0;
  this->caller_route_.clear();
  this->caller_name_.clear();
  this->dest_route_.clear();
  this->dest_name_.clear();
  this->close_media_session_();
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  this->cancel_requested_.store(false, std::memory_order_release);
  this->clear_udp_transactions_();
  this->reset_rtp_latch_();
}

void SipTransport::remember_udp_transaction_(const std::string &method, const std::string &message,
                                             uint32_t ip_v4, uint16_t port) {
  if (this->remote_sip_tcp_.load(std::memory_order_acquire) || message.empty() || ip_v4 == 0 || port == 0) {
    return;
  }
  UdpTransaction *txn = nullptr;
  if (method == "INVITE") {
    txn = &this->pending_invite_;
  } else if (method == "CANCEL") {
    txn = &this->pending_cancel_;
  } else if (method == "BYE") {
    txn = &this->pending_bye_;
  }
  if (txn == nullptr) return;
  const uint32_t now = millis();
  txn->request = message;
  txn->ip_v4 = ip_v4;
  txn->port = port;
  txn->interval_ms = SIP_T1_MS;
  txn->next_ms = now + txn->interval_ms;
  txn->deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  txn->retries = 0;
  txn->completed = false;
  this->wake_sip_task_();
}

void SipTransport::pump_udp_retransmits_() {
  const uint32_t now = millis();
  bool reset_terminal_dialog = false;
  bool invite_timed_out = false;
  bool ack_timed_out = false;
  std::string timed_out_call_id;
  LockGuard lock(this->dialog_mutex_);
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    // A cached UDP server transaction must never be replayed over a later TCP
    // dialog. Reactive duplicate handling remains available in the cache.
    if (this->completed_invite_.udp) this->completed_invite_.awaiting_ack = false;
    return;
  }
  auto pump = [this, now, &reset_terminal_dialog, &invite_timed_out,
               &timed_out_call_id](UdpTransaction &txn, const char *method) {
    if (txn.empty() || txn.ip_v4 == 0 || txn.port == 0) return;
    if (time_reached(now, txn.deadline_ms)) {
      ESP_LOGW(TAG, "SIP UDP %s transaction timed out after %u ms", method,
               (unsigned) SIP_TRANSACTION_TIMEOUT_MS);
      txn.clear();
      if (std::strcmp(method, "INVITE") == 0) {
        invite_timed_out = true;
        timed_out_call_id = this->call_id_;
        this->outgoing_invite_pending_.store(false, std::memory_order_release);
      } else {
        reset_terminal_dialog = true;
      }
      return;
    }
    if (!time_reached(now, txn.next_ms)) return;
    if (txn.completed) {
      txn.next_ms = txn.deadline_ms;
      return;
    }
    const bool sent = this->send_sip_(txn.request, txn.ip_v4, txn.port);
    txn.retries++;
    if (sent) {
      ESP_LOGD(TAG, "SIP UDP %s retransmit #%u", method, (unsigned) txn.retries);
    } else {
      ESP_LOGW(TAG, "SIP UDP %s retransmit #%u failed", method, (unsigned) txn.retries);
    }
    txn.interval_ms = std::min<uint16_t>(static_cast<uint16_t>(txn.interval_ms * 2), SIP_T2_MS);
    txn.next_ms = now + txn.interval_ms;
    if (time_reached(txn.next_ms, txn.deadline_ms)) txn.next_ms = txn.deadline_ms;
  };

  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    pump(this->pending_invite_, "INVITE");
  }
  pump(this->pending_cancel_, "CANCEL");
  pump(this->pending_bye_, "BYE");
  if (this->completed_invite_.udp && this->completed_invite_.awaiting_ack) {
    if (time_reached(now, this->completed_invite_.deadline_ms)) {
      ESP_LOGW(TAG, "SIP UDP INVITE final response timed out waiting for ACK after %u ms",
               (unsigned) SIP_TRANSACTION_TIMEOUT_MS);
      this->completed_invite_.awaiting_ack = false;
      const bool active_2xx_dialog = this->completed_invite_.status < 300 &&
                                     !this->call_id_.empty() &&
                                     this->call_id_ == this->completed_invite_.call_id &&
                                     this->last_invite_cseq_number_ == this->completed_invite_.cseq;
      if (active_2xx_dialog) {
        ack_timed_out = true;
        timed_out_call_id = this->call_id_;
      }
    } else if (time_reached(now, this->completed_invite_.next_retransmit_ms)) {
      const bool sent = this->send_sip_(this->completed_invite_.response,
                                        this->completed_invite_.peer_ip_v4,
                                        this->completed_invite_.peer_port);
      this->completed_invite_.retransmits++;
      if (sent) {
        ESP_LOGD(TAG, "SIP UDP INVITE final response retransmit #%u",
                 (unsigned) this->completed_invite_.retransmits);
      } else {
        ESP_LOGW(TAG, "SIP UDP INVITE final response retransmit #%u failed",
                 (unsigned) this->completed_invite_.retransmits);
      }
      this->completed_invite_.retransmit_interval_ms =
          std::min<uint16_t>(static_cast<uint16_t>(this->completed_invite_.retransmit_interval_ms * 2),
                             SIP_T2_MS);
      this->completed_invite_.next_retransmit_ms =
          now + this->completed_invite_.retransmit_interval_ms;
      if (time_reached(this->completed_invite_.next_retransmit_ms,
                       this->completed_invite_.deadline_ms)) {
        this->completed_invite_.next_retransmit_ms = this->completed_invite_.deadline_ms;
      }
    }
  }
  if (reset_terminal_dialog) {
    this->reset_dialog_();
  }
  if (invite_timed_out) {
    SipSignal signal;
    signal.type = SipSignalType::FINAL_RESPONSE;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "timeout";
    this->reset_dialog_();
    this->emit_sip_signal_(signal);
  }
  if (ack_timed_out) {
    SipSignal signal;
    signal.type = SipSignalType::PROTOCOL_ERROR;
    signal.status_code = 408;
    signal.call_id = timed_out_call_id;
    signal.reason = "ack_timeout";
    this->reset_dialog_();
    this->emit_sip_signal_(signal);
  }
}

bool SipTransport::local_ip_for_peer_(uint32_t peer_ip_v4, std::string *out) const {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  bool ok = false;
  if (fd >= 0) {
    struct sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(9);
    peer.sin_addr.s_addr = htonl(peer_ip_v4);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&peer), sizeof(peer)) == 0) {
      struct sockaddr_in local{};
      socklen_t len = sizeof(local);
      if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&local), &len) == 0 &&
          local.sin_addr.s_addr != 0) {
        char ip[16];
        inet_ntoa_r(local.sin_addr, ip, sizeof(ip));
        *out = ip;
        ok = true;
      }
    }
    close(fd);
  }
  if (ok) return true;
  char ip[network::IP_ADDRESS_BUFFER_SIZE];
  for (auto &address : network::get_ip_addresses()) {
    if (!address.is_ip4()) continue;
    address.str_to(ip);
    if (std::strcmp(ip, "0.0.0.0") != 0) {
      *out = ip;
      ESP_LOGW(TAG, "SIP local IP selected %s for peer %08x", ip, (unsigned) peer_ip_v4);
      return true;
    }
  }
  return false;
}

bool SipTransport::send_sip_(const std::string &message, uint32_t ip_v4, uint16_t port) {
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    return this->send_sip_tcp_(message);
  }
  if (this->sip_socket_ < 0 || ip_v4 == 0 || port == 0) return false;
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip_v4);
  dest.sin_port = htons(port);
  const int sent = sendto(this->sip_socket_, message.data(), message.size(), 0,
                          reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  if (sent != static_cast<int>(message.size())) {
    const int err = errno;
    ESP_LOGW(TAG, "SIP TX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
    return false;
  }
  char ip[16];
  struct in_addr a{};
  a.s_addr = htonl(ip_v4);
  inet_ntoa_r(a, ip, sizeof(ip));
  ESP_LOGI(TAG, "SIP TX %u bytes to %s:%u", (unsigned) message.size(), ip, (unsigned) port);
  return true;
}

bool SipTransport::send_sip_tcp_(const std::string &message) {
  // SIP commands can originate from the component/main task while the SIP
  // task sends responses. Serialize the complete record so partial TCP writes
  // from two callers can never interleave into an invalid SIP message.
  LockGuard send_lock(this->tcp_send_mutex_);
  if (message.empty()) return false;
  const bool replacing_session =
      this->sip_tcp_client_close_requested_.load(std::memory_order_acquire) ||
      this->tcp_connect_requested_.load(std::memory_order_acquire);
  const int socket = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
  if (socket < 0 || replacing_session) {
    if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
      {
        LockGuard lock(this->tcp_tx_pending_mutex_);
        if (!this->tcp_tx_pending_.empty()) {
          ESP_LOGW(TAG, "SIP TCP connect still pending; refusing to replace queued %s with a newer request",
                   this->tcp_tx_pending_.rfind("INVITE ", 0) == 0 ? "INVITE" : "message");
          return false;
        }
        this->tcp_tx_pending_ = message;
      }
      this->wake_sip_task_();
      return true;
    }
    return false;
  }
  return this->send_sip_tcp_record_(message, socket);
}

bool SipTransport::send_sip_tcp_record_(const std::string &message, int socket) {
  // Caller owns tcp_send_mutex_. Keep this primitive allocation-free so the
  // SIP task can flush the one queued record atomically when connect finishes.
  if (message.empty() || socket < 0) return false;
  size_t sent_total = 0;
  while (sent_total < message.size()) {
    const int sent = send(socket, message.data() + sent_total, message.size() - sent_total, 0);
    if (sent <= 0) {
      const int err = errno;
      ESP_LOGW(TAG, "SIP TCP TX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
      // A partial SIP record cannot be resumed by a later caller. Closing the
      // stream lets the SIP task perform one coherent dialog teardown.
      shutdown(socket, SHUT_RDWR);
      return false;
    }
    sent_total += static_cast<size_t>(sent);
  }
  ESP_LOGI(TAG, "SIP TCP TX %u bytes", (unsigned) message.size());
  return true;
}

std::string SipTransport::wrap_sdp_envelope_(const std::string &local_ip, const std::string &payloads,
                                             const std::string &maps, const std::string &flows,
                                             uint8_t ptime) const {
  return "v=0\r\n"
         "o=- 0 0 IN IP4 " + local_ip + "\r\n"
         "s=VoIP Stack\r\n"
         "c=IN IP4 " + local_ip + "\r\n"
         "t=0 0\r\n"
         "m=audio " + std::to_string(this->rtp_port_) + " RTP/AVP " + payloads + "\r\n" +
         maps +
         flows +
         "a=ptime:" + std::to_string(ptime) + "\r\n"
         "a=maxptime:" + std::to_string(ptime) + "\r\n"
         "a=sendrecv\r\n";
}

std::string SipTransport::build_sdp_offer_() const {
  const uint32_t remote_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(remote_ip, &local_ip);
  std::string payloads;
  std::string maps;
  std::string flows;
  uint8_t pt = 96;
  const uint8_t selected_ptime =
      choose_common_audio_ptime(this->offer_tx_formats_, this->offer_rx_formats_, this->udp_max_payload_);
  if (selected_ptime == 0) {
    ESP_LOGW(TAG, "SIP SDP offer has no shared TX/RX RTP packet time");
    return "";
  }
  auto append_format = [&](const AudioFormat &fmt, const char *flow) {
    if (pt >= 120) return;
    if (fmt.frame_ms != selected_ptime) return;
    const char *enc = audio_format_rtp_encoding(fmt, this->udp_max_payload_);
    if (enc == nullptr) return;
    if (!payloads.empty()) payloads.push_back(' ');
    payloads += std::to_string(pt);
    maps += "a=rtpmap:" + std::to_string(pt) + " " + enc + "/" +
            std::to_string(fmt.sample_rate) + "/" + std::to_string(fmt.channels) + "\r\n";
    flows += "a=x-voip-stack-flow:" + std::to_string(pt) + " " + flow + "\r\n";
    pt++;
  };
  for (uint8_t i = 0; i < this->offer_rx_formats_.count && pt < 120; i++) {
    append_format(this->offer_rx_formats_.formats[i],
                  audio_format_list_contains(this->offer_tx_formats_, this->offer_rx_formats_.formats[i])
                      ? "sendrecv" : "recv");
  }
  for (uint8_t i = 0; i < this->offer_tx_formats_.count && pt < 120; i++) {
    if (audio_format_list_contains(this->offer_rx_formats_, this->offer_tx_formats_.formats[i])) continue;
    append_format(this->offer_tx_formats_.formats[i], "send");
  }
  if (payloads.empty()) {
    ESP_LOGW(TAG, "SIP SDP offer has no common UDP-safe RTP PCM format");
    return "";
  }
  return this->wrap_sdp_envelope_(local_ip, payloads, maps, flows, selected_ptime);
}

std::string SipTransport::build_sdp_answer_() const {
  const uint32_t remote_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(remote_ip, &local_ip);
  AudioFormat selected_tx;
  AudioFormat selected_rx;
  uint8_t tx_payload_type = 96;
  uint8_t rx_payload_type = 96;
  this->get_media_config_(&selected_tx, &selected_rx, &tx_payload_type, &rx_payload_type);
  const char *tx_enc = audio_format_rtp_encoding(selected_tx, this->udp_max_payload_);
  const char *rx_enc = audio_format_rtp_encoding(selected_rx, this->udp_max_payload_);
  if (tx_enc == nullptr || rx_enc == nullptr) {
    ESP_LOGE(TAG, "SIP SDP answer refused: selected wire PCM format is not RTP-mappable");
    return "";
  }
  std::string payloads = std::to_string(rx_payload_type);
  std::string maps;
  std::string flows;
  maps += "a=rtpmap:" + std::to_string(rx_payload_type) + " " + rx_enc + "/" +
          std::to_string(selected_rx.sample_rate) + "/" +
          std::to_string(selected_rx.channels) + "\r\n";
  flows += "a=x-voip-stack-flow:" + std::to_string(rx_payload_type) +
           (tx_payload_type == rx_payload_type ? " sendrecv\r\n" : " recv\r\n");
  if (tx_payload_type != rx_payload_type) {
    payloads += " " + std::to_string(tx_payload_type);
    maps += "a=rtpmap:" + std::to_string(tx_payload_type) + " " + tx_enc + "/" +
          std::to_string(selected_tx.sample_rate) + "/" +
          std::to_string(selected_tx.channels) + "\r\n";
    flows += "a=x-voip-stack-flow:" + std::to_string(tx_payload_type) + " send\r\n";
  }
  return this->wrap_sdp_envelope_(local_ip, payloads, maps, flows, selected_rx.frame_ms);
}

bool SipTransport::learn_remote_rtp_from_sdp_(const std::string &sdp, uint32_t default_ip) {
  uint16_t media_port = 0;
  uint32_t media_ip = default_ip;
  uint8_t media_ptime = 20;
  bool selected_tx = false;
  bool selected_rx = false;
  AudioFormat selected_tx_format;
  AudioFormat selected_rx_format;
  uint8_t selected_tx_payload_type = 0;
  uint8_t selected_rx_payload_type = 0;
  uint8_t payload_flow[128]{};
  bool offered_payload[128]{};
  uint8_t session_flow = 0x03;
  uint8_t media_flow = session_flow;
  size_t selected_audio_line = std::string::npos;
  bool seen_any_media = false;
  bool in_audio = false;
  size_t ptime_pos = 0;
  while (ptime_pos < sdp.size()) {
    size_t end = sdp.find("\r\n", ptime_pos);
    if (end == std::string::npos) end = sdp.size();
    const std::string line = sdp.substr(ptime_pos, end - ptime_pos);
    if (line.rfind("m=", 0) == 0) {
      seen_any_media = true;
      in_audio = false;
      if (selected_audio_line == std::string::npos) {
        bool candidate_payload[128]{};
        uint16_t candidate_port = 0;
        if (parse_audio_media_line(line, &candidate_port, candidate_payload)) {
          media_port = candidate_port;
          std::copy(candidate_payload, candidate_payload + 128, offered_payload);
          selected_audio_line = ptime_pos;
          media_flow = session_flow;
          in_audio = true;
        }
      }
    } else if (in_audio && line.rfind("a=ptime:", 0) == 0) {
      uint32_t parsed = 0;
      if (parse_decimal_u32(line.substr(8), UINT8_MAX, &parsed) &&
          (parsed == 10 || parsed == 16 || parsed == 20 || parsed == 32)) {
        media_ptime = static_cast<uint8_t>(parsed);
      }
    } else if (in_audio && line.rfind("a=x-voip-stack-flow:", 0) == 0) {
      const size_t value_start = sizeof("a=x-voip-stack-flow:") - 1;
      const size_t space = line.find(' ', value_start);
      if (space != std::string::npos) {
        uint32_t parsed_pt = 0;
        const std::string flow = trim_copy(line.substr(space + 1));
        if (parse_decimal_u32(line.substr(value_start, space - value_start), 127, &parsed_pt)) {
          uint8_t flags = 0;
          if (flow == "send" || flow == "sendrecv") flags |= 0x01;
          if (flow == "recv" || flow == "sendrecv") flags |= 0x02;
          payload_flow[parsed_pt] = flags;
        }
      }
    } else if ((!seen_any_media || in_audio) && line == "a=sendonly") {
      if (!seen_any_media) session_flow = 0x01;
      if (in_audio) media_flow = 0x01;
    } else if ((!seen_any_media || in_audio) && line == "a=recvonly") {
      if (!seen_any_media) session_flow = 0x02;
      if (in_audio) media_flow = 0x02;
    } else if ((!seen_any_media || in_audio) && line == "a=inactive") {
      if (!seen_any_media) session_flow = 0;
      if (in_audio) media_flow = 0;
    } else if ((!seen_any_media || in_audio) && line == "a=sendrecv") {
      if (!seen_any_media) session_flow = 0x03;
      if (in_audio) media_flow = 0x03;
    }
    if (end == sdp.size()) break;
    ptime_pos = end + 2;
  }
  size_t pos = 0;
  bool seen_media = false;
  in_audio = false;
  while (pos < sdp.size()) {
    size_t end = sdp.find("\r\n", pos);
    if (end == std::string::npos) end = sdp.size();
    const std::string line = sdp.substr(pos, end - pos);
    if (line.rfind("m=", 0) == 0) {
      seen_media = true;
      in_audio = pos == selected_audio_line;
    } else if (line.rfind("c=IN IP4 ", 0) == 0 && (!seen_media || in_audio)) {
      struct in_addr a{};
      if (inet_aton(line.substr(9).c_str(), &a) != 0 && a.s_addr != 0) media_ip = ntohl(a.s_addr);
    } else if (in_audio && line.rfind("a=rtpmap:", 0) == 0) {
      AudioFormat fmt;
      uint8_t pt = 0;
      if (parse_rtpmap_format(line, &fmt, &pt) && offered_payload[pt]) {
        fmt.frame_ms = media_ptime;
        AudioFormat local_rx;
        AudioFormat local_tx;
        const uint8_t flow = payload_flow[pt] == 0 ? media_flow : payload_flow[pt];
        const bool peer_can_send = (flow & 0x01) != 0;
        const bool peer_can_recv = (flow & 0x02) != 0;
        const bool tx_ok = peer_can_recv &&
                           audio_format_list_match_udp_safe(this->offer_tx_formats_, fmt, &local_tx,
                                                            this->udp_max_payload_);
        const bool rx_ok = peer_can_send &&
                           audio_format_list_match_udp_safe(this->offer_rx_formats_, fmt, &local_rx,
                                                            this->udp_max_payload_);
        if (!selected_rx && rx_ok) {
          selected_rx_format = local_rx;
          selected_rx_payload_type = pt;
          selected_rx = true;
          ESP_LOGI(TAG, "SIP SDP selected RX PT=%u L%u/%u/%u frame=%ums",
                   (unsigned) pt,
                   fmt.pcm_format == PcmFormat::S24LE ? 24u : 16u,
                   (unsigned) selected_rx_format.sample_rate,
                   (unsigned) selected_rx_format.channels,
                   (unsigned) selected_rx_format.frame_ms);
        }
        if (!selected_tx && tx_ok) {
          selected_tx_format = local_tx;
          selected_tx_payload_type = pt;
          selected_tx = true;
          ESP_LOGI(TAG, "SIP SDP selected TX PT=%u L%u/%u/%u frame=%ums",
                   (unsigned) pt,
                   fmt.pcm_format == PcmFormat::S24LE ? 24u : 16u,
                   (unsigned) selected_tx_format.sample_rate,
                   (unsigned) selected_tx_format.channels,
                   (unsigned) selected_tx_format.frame_ms);
        } else if (!selected_tx && !selected_rx) {
          ESP_LOGD(TAG, "SIP SDP skipping unsupported PT=%u rate=%u pcm=%u channels=%u",
                   (unsigned) pt,
                   (unsigned) fmt.sample_rate,
                   (unsigned) fmt.pcm_format,
                   (unsigned) fmt.channels);
        }
      }
    }
    if (end == sdp.size()) break;
    pos = end + 2;
  }
  if (media_port == 0 || media_ip == 0 || !selected_tx || !selected_rx) {
    ESP_LOGW(TAG,
             "SIP SDP rejected: body_len=%u media_port=%u media_ip=%08x "
             "selected_tx=%s selected_rx=%s",
             (unsigned) sdp.size(), (unsigned) media_port, (unsigned) media_ip, selected_tx ? "yes" : "no",
             selected_rx ? "yes" : "no");
    return false;
  }
  if (selected_tx_format.frame_ms != selected_rx_format.frame_ms) {
    ESP_LOGW(TAG, "SIP SDP rejected: TX/RX ptime mismatch tx=%ums rx=%ums",
             (unsigned) selected_tx_format.frame_ms,
             (unsigned) selected_rx_format.frame_ms);
    return false;
  }
  this->set_media_config_(selected_tx_format, selected_rx_format,
                          selected_tx_payload_type, selected_rx_payload_type);
  // Signaling can traverse a PBX/proxy while RTP terminates on a separate
  // media address from SDP. Never overwrite the SIP peer with the media peer.
  this->remote_rtp_ip_v4_.store(media_ip, std::memory_order_release);
  this->remote_rtp_port_.store(media_port, std::memory_order_release);
  return true;
}

bool SipTransport::send_request_(const std::string &method, const std::string &body) {
  SipRequestOptions options;
  return this->send_request_(method, body, options);
}

bool SipTransport::send_request_(const std::string &method, const std::string &body,
                                 const SipRequestOptions &options) {
  uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  uint16_t port = this->remote_sip_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0 || this->call_id_.empty()) return false;
  std::string branch;
  if (!options.branch_override.empty()) {
    branch = options.branch_override;
  } else if (method == "INVITE") {
    if (this->branch_.empty()) this->branch_ = "z9hG4bK" + make_token("");
    branch = this->branch_;
  } else if (method == "CANCEL") {
    if (this->branch_.empty()) this->branch_ = "z9hG4bK" + make_token("");
    branch = this->branch_;
  } else {
    branch = "z9hG4bK" + make_token("");
  }
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(ip, &local_ip);
  std::string request_uri = this->remote_target_uri_;
  if (request_uri.empty()) request_uri = strip_angle_uri(this->remote_uri_);
  if (request_uri.empty()) {
    struct in_addr remote_address{};
    remote_address.s_addr = htonl(ip);
    char remote_ip[16];
    inet_ntoa_r(remote_address, remote_ip, sizeof(remote_ip));
    request_uri = "sip:voip@" + std::string(remote_ip);
  }
  // A confirmed dialog is retargeted by Contact. For UDP the Request-URI and
  // actual next hop must agree; retaining the original port here breaks peers
  // whose Contact listens on a different socket. TCP keeps using its existing
  // connection, so the parsed destination is intentionally UDP-only.
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    uint32_t target_ip = 0;
    uint16_t target_port = 0;
    if (sip_uri_ipv4_target(request_uri, &target_ip, &target_port)) {
      ip = target_ip;
      port = target_port;
    }
  }
  const char *transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "TCP" : "UDP";
  std::string msg = method + " " + request_uri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/" + std::string(transport) + " " + local_ip + ":" +
         std::to_string(this->sip_port_) + ";branch=" + branch + ";rport\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: " + this->local_uri_ + ";tag=" + this->local_tag_ + "\r\n";
  msg += "To: " + this->remote_uri_;
  if (!this->remote_tag_.empty()) msg += ";tag=" + this->remote_tag_;
  msg += "\r\n";
  msg += "Call-ID: " + this->call_id_ + "\r\n";
  const uint32_t request_cseq = options.cseq_number == 0 ? this->cseq_++ : options.cseq_number;
  const std::string cseq_method = options.cseq_method.empty() ? method : options.cseq_method;
  msg += "CSeq: " + std::to_string(request_cseq) + " " + cseq_method + "\r\n";
  msg += "Contact: " + this->local_uri_ + "\r\n";
  msg += "User-Agent: ESPHome-VoIP-Stack-SIP\r\n";
  if (method == "INVITE") {
    msg += "X-Voip-Stack-Caller-Route: " + sip_header_token(this->caller_route_, VOIP_STACK_MAX_ROUTE_ID_LEN) + "\r\n";
    msg += "X-Voip-Stack-Caller-Name: " + sip_header_token(this->caller_name_, VOIP_STACK_MAX_NAME_LEN) + "\r\n";
    msg += "X-Voip-Stack-Dest-Route: " + sip_header_token(this->dest_route_, VOIP_STACK_MAX_ROUTE_ID_LEN) + "\r\n";
    msg += "X-Voip-Stack-Dest-Name: " + sip_header_token(this->dest_name_, VOIP_STACK_MAX_NAME_LEN) + "\r\n";
  }
  if (!body.empty()) msg += "Content-Type: application/sdp\r\n";
  msg += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  msg += body;
  if (method == "ACK" && !this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    this->remember_completed_invite_ack_(msg, ip, port);
  }
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent) {
    const SipEvent event = sip_event_from_method_(method);
    if (event != SipEvent::NONE) this->mark_sip_event_(event);
    if (method == "INVITE" || method == "CANCEL" || method == "BYE") {
      this->remember_udp_transaction_(method, msg, ip, port);
    }
  }
  return sent;
}

bool SipTransport::send_invite_error_ack_() {
  if (this->branch_.empty()) return false;
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
  options.cseq_method = "ACK";
  // A non-2xx INVITE ACK reuses the INVITE client transaction branch per RFC
  // 3261 section 17.1.1.3.
  options.branch_override = this->branch_;
  return this->send_request_("ACK", "", options);
}

std::string SipTransport::format_response_(uint16_t status, const char *reason,
                                            const std::string &via, const std::string &from,
                                            const std::string &to, const std::string &call_id,
                                            const std::string &cseq, const std::string &app_reason,
                                            const std::string &body, bool add_contact_ua,
                                            bool add_to_tag, bool stateless) {
  std::string msg = "SIP/2.0 " + std::to_string(status) + " " + reason + "\r\n";
  msg += "Via: " + via + "\r\n";
  msg += "From: " + from + "\r\n";
  msg += "To: " + to;
  if (add_to_tag && status != 100 && tag_from_header(to).empty()) {
    if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
    msg += ";tag=" + this->local_tag_;
  }
  msg += "\r\n";
  msg += "Call-ID: " + call_id + "\r\n";
  msg += "CSeq: " + cseq + "\r\n";
  if (add_contact_ua) {
    msg += "Contact: " + this->local_uri_ + "\r\n";
    msg += "User-Agent: ESPHome-VoIP-Stack-SIP\r\n";
  }
  const std::string clean_reason = sip_header_token(app_reason);
  if (!clean_reason.empty() && (!stateless || status >= 300)) {
    msg += "Reason: X-Voip-Stack;cause=" + std::to_string(status) + ";text=" + sip_quoted(clean_reason) + "\r\n";
    msg += "X-Voip-Stack-Decline-Reason: " + clean_reason + "\r\n";
  }
  if (status == 405) {
    msg += "Allow: ACK, BYE, CANCEL, INVITE, OPTIONS\r\n";
  }
  if (!body.empty()) msg += "Content-Type: application/sdp\r\n";
  msg += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  msg += body;
  return msg;
}

bool SipTransport::send_response_(uint16_t status, const char *reason, const std::string &body,
                                  const std::string &app_reason) {
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_sip_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0 || this->last_invite_via_.empty()) return false;
  const std::string msg = this->format_response_(
      status, reason, response_via_with_rport(this->last_invite_via_, ip, port),
      this->last_invite_from_, this->last_invite_to_, this->call_id_,
      this->last_invite_cseq_, app_reason, body, true, true, false);
  this->last_invite_response_ = msg;
  if (status >= 200) {
    this->remember_completed_response_("Via: " + this->last_invite_via_ + "\r\nCall-ID: " + this->call_id_ +
                                           "\r\nCSeq: " + this->last_invite_cseq_ + "\r\n",
                                       ip, port, "INVITE", msg);
  }
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent) this->mark_sip_event_(SipEvent::RESPONSE, status);
  return sent;
}

bool SipTransport::send_stateless_response_(const std::string &request, const sockaddr_in &src,
                                            uint16_t status, const char *reason,
                                            const std::string &app_reason,
                                            bool cache_transaction) {
  const uint32_t ip = ntohl(src.sin_addr.s_addr);
  const uint16_t port = ntohs(src.sin_port);
  const std::string via = header_values(request, "Via");
  const std::string from = header_value(request, "From");
  const std::string to = header_value(request, "To");
  const std::string call_id = header_value(request, "Call-ID");
  const std::string cseq = header_value(request, "CSeq");
  if (via.empty() || from.empty() || to.empty() || call_id.empty() || cseq.empty()) return false;
  std::string response_to = to;
  const std::string method = cseq_method(cseq);
  if (cache_transaction && tag_from_header(response_to).empty()) {
    const std::string tag = (method == "INVITE" || this->local_tag_.empty())
                                ? make_token("tag")
                                : this->local_tag_;
    response_to += ";tag=" + tag;
  }
  const std::string msg = this->format_response_(
      status, reason, response_via_with_rport(via, ip, port),
      from, response_to, call_id, cseq, app_reason, "", false, false, true);
  if (cache_transaction && status >= 200) {
    this->remember_completed_response_(request, ip, port, method, msg);
  }
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent) this->mark_sip_event_(SipEvent::RESPONSE, status);
  return sent;
}

bool SipTransport::send_invite(const std::string &call_id,
                               const std::string &caller_route,
                               const std::string &caller_name,
                               const std::string &dest_route,
                               const std::string &dest_name) {
  LockGuard lock(this->dialog_mutex_);
  // A fresh INVITE must never inherit tags or transaction state from a
  // cancelled dialog whose final response was lost.
  this->reset_dialog_();
  this->call_id_ = call_id;
  this->caller_route_ = caller_route;
  this->caller_name_ = caller_name;
  this->dest_route_ = dest_route;
  this->dest_name_ = dest_name;
  this->last_sip_status_code_.store(0, std::memory_order_release);
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (ip == 0) {
    this->reset_dialog_();
    return false;
  }
  if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
  this->branch_ = "z9hG4bK" + make_token("");
  this->invite_cseq_ = this->cseq_;
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(ip, &local_ip);
  struct in_addr a{};
  a.s_addr = htonl(ip);
  char ip_text[16];
  inet_ntoa_r(a, ip_text, sizeof(ip_text));
  const char *uri_transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "tcp" : "udp";
  this->local_uri_ = "<sip:" + sip_uri_user_encode(this->caller_name_) + "@" + local_ip + ":" +
                     std::to_string(this->sip_port_) + ";transport=" + uri_transport + ">";
  this->remote_uri_ = "<sip:" + sip_uri_user_encode(this->dest_name_) + "@" + std::string(ip_text) + ":" +
                      std::to_string(this->remote_sip_port_.load(std::memory_order_acquire)) +
                      ";transport=" + uri_transport + ">";
  this->remote_target_uri_ = strip_angle_uri(this->remote_uri_);
  ESP_LOGI(TAG, "SIP INVITE call_id=%s from=%s to=%s", this->call_id_.c_str(),
           this->caller_name_.c_str(), this->dest_name_.c_str());
  const std::string sdp = this->build_sdp_offer_();
  if (sdp.empty()) {
    this->reset_dialog_();
    return false;
  }
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
  const bool sent = this->send_request_("INVITE", sdp, options);
  if (sent) {
    this->outgoing_invite_pending_.store(true, std::memory_order_release);
    if (this->cseq_ <= this->invite_cseq_) this->cseq_ = this->invite_cseq_ + 1;
  } else {
    this->reset_dialog_();
  }
  return sent;
}

void SipTransport::send_audio_frame(const uint8_t *pcm, size_t bytes) {
  if (!this->rtp_running_.load(std::memory_order_acquire) || pcm == nullptr || bytes == 0) return;
  // PCM conversion is the expensive part. Keep it outside the socket mutex;
  // teardown flips rtp_running_ first and the short final critical section
  // below prevents close-vs-send without delaying the media loop.
  AudioFormat tx_format;
  uint8_t tx_payload_type = 96;
  this->get_media_config_(&tx_format, nullptr, &tx_payload_type, nullptr);
  uint8_t packet[1500];
  const uint8_t bps = tx_format.container_bytes_per_sample();
  const size_t input_bytes = bytes;
  const uint32_t samples = bps == 0 || tx_format.channels == 0
      ? 0
      : static_cast<uint32_t>(input_bytes / bps / tx_format.channels);
  bytes = pcm_to_rtp_payload(pcm, bytes, tx_format, packet + 12, sizeof(packet) - 12);
  if (bytes == 0 || bytes > this->udp_max_payload_) {
    // Sequence numbers count packets, while timestamps follow the sampling
    // clock. A locally discarded PCM frame therefore advances only time.
    this->rtp_timestamp_.fetch_add(samples, std::memory_order_acq_rel);
    return;
  }
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (!this->rtp_running_.load(std::memory_order_acquire) || this->rtp_socket_ < 0) return;
  const uint32_t ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_rtp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return;
  packet[0] = 0x80;
  packet[1] = tx_payload_type & 0x7F;
  const uint16_t seq = this->rtp_sequence_.fetch_add(1, std::memory_order_acq_rel);
  packet[2] = static_cast<uint8_t>(seq >> 8);
  packet[3] = static_cast<uint8_t>(seq & 0xFF);
  const uint32_t ts = this->rtp_timestamp_.fetch_add(samples, std::memory_order_acq_rel);
  packet[4] = static_cast<uint8_t>(ts >> 24);
  packet[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
  packet[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
  packet[7] = static_cast<uint8_t>(ts & 0xFF);
  packet[8] = static_cast<uint8_t>(this->rtp_ssrc_ >> 24);
  packet[9] = static_cast<uint8_t>((this->rtp_ssrc_ >> 16) & 0xFF);
  packet[10] = static_cast<uint8_t>((this->rtp_ssrc_ >> 8) & 0xFF);
  packet[11] = static_cast<uint8_t>(this->rtp_ssrc_ & 0xFF);
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip);
  dest.sin_port = htons(port);
  if (!this->rtp_running_.load(std::memory_order_acquire)) return;
  const int sent = sendto(this->rtp_socket_, packet, 12 + bytes, 0,
                          reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  if (sent > 0) {
    this->rtp_tx_packets_.fetch_add(1, std::memory_order_acq_rel);
    this->rtp_tx_bytes_.fetch_add(static_cast<uint32_t>(sent), std::memory_order_acq_rel);
  }
}

bool SipTransport::send_ringing(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty()) this->call_id_ = call_id;
  return this->send_response_(180, "Ringing");
}

bool SipTransport::send_answer(const std::string &call_id,
                               const AudioFormat &caller_to_dest_format,
                               const AudioFormat &dest_to_caller_format) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty()) this->call_id_ = call_id;
  uint8_t tx_payload_type = 96;
  uint8_t rx_payload_type = 96;
  this->get_media_config_(nullptr, nullptr, &tx_payload_type, &rx_payload_type);
  this->set_media_config_(dest_to_caller_format, caller_to_dest_format,
                          tx_payload_type, rx_payload_type);
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
  const std::string answer = this->build_sdp_answer_();
  if (answer.empty()) {
    const bool sent = this->send_response_(488, "Not Acceptable Here", "", "media_incompatible");
    this->reset_dialog_();
    return sent;
  }
  const bool sent = this->send_response_(200, "OK", answer);
  if (sent) this->open_media_session_();
  return sent;
}

bool SipTransport::send_cancel(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_cancel_unlocked_(call_id);
}

bool SipTransport::send_cancel_unlocked_(const std::string &call_id) {
  if (!call_id.empty()) this->call_id_ = call_id;
  if (!this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_bye_unlocked_(call_id);
  }
  SipRequestOptions options;
  options.cseq_number = this->invite_cseq_;
  this->cancel_requested_.store(true, std::memory_order_release);
  const bool sent = this->send_request_("CANCEL", "", options);
  if (sent) {
    // INVITE retransmission stops once cancellation begins. Keep the dialog
    // identifiers until the final 487 so it can be ACKed correctly.
    this->clear_invite_transaction_();
  } else {
    this->cancel_requested_.store(false, std::memory_order_release);
  }
  return sent;
}

bool SipTransport::send_bye(const std::string &call_id) {
  LockGuard lock(this->dialog_mutex_);
  return this->send_bye_unlocked_(call_id);
}

bool SipTransport::send_bye_unlocked_(const std::string &call_id) {
  if (!call_id.empty()) this->call_id_ = call_id;
  this->stop_audio_path();
  return this->send_request_("BYE");
}

bool SipTransport::send_final_response(const std::string &call_id,
                                       uint16_t status,
                                       const std::string &reason) {
  LockGuard lock(this->dialog_mutex_);
  if (!call_id.empty()) this->call_id_ = call_id;
  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_cancel_unlocked_(call_id);
  }
  const char *phrase = "Busy Here";
  if (status == 603) phrase = "Decline";
  else if (status == 488) phrase = "Not Acceptable Here";
  else if (status == 487) phrase = "Request Terminated";
  else if (status == 500) phrase = "Server Internal Error";
  const bool sent = this->send_response_(status, phrase, "", reason);
  this->reset_dialog_();
  return sent;
}

bool SipTransport::replay_completed_response_(const std::string &request, const sockaddr_in &src,
                                              const std::string &method) {
  CompletedServerTransaction *completed = method == "INVITE" ? &this->completed_invite_
                                                               : &this->completed_control_;
  if (completed->empty()) return false;
  if (millis() - completed->completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    completed->clear();
    return false;
  }
  const std::string call_id = header_value(request, "Call-ID");
  const std::string cseq = header_value(request, "CSeq");
  const std::string branch = via_branch(header_value(request, "Via"));
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const bool transport_matches =
      completed->udp == !this->remote_sip_tcp_.load(std::memory_order_acquire);
  if (completed->method != method || completed->call_id != call_id ||
      completed->cseq != cseq_number(cseq) || cseq_method(cseq) != method ||
      completed->peer_ip_v4 != peer_ip || !transport_matches ||
      (!completed->branch.empty() && completed->branch != branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP %s retransmission replaying cached response for call_id=%s",
           method.c_str(), call_id.c_str());
  this->send_sip_(completed->response, peer_ip, ntohs(src.sin_port));
  return true;
}

void SipTransport::remember_completed_response_(const std::string &request, uint32_t peer_ip_v4,
                                                uint16_t peer_port,
                                                const std::string &method,
                                                const std::string &response) {
  if (response.empty() || peer_ip_v4 == 0 || peer_port == 0 ||
      (method != "INVITE" && method != "CANCEL" && method != "BYE") ||
      response.rfind("SIP/2.0 ", 0) != 0 || response.size() < 12) {
    return;
  }
  uint32_t parsed_status = 0;
  if (!parse_decimal_u32(response.substr(8, 3), 699, &parsed_status) || parsed_status < 200) return;
  CompletedServerTransaction *completed = method == "INVITE" ? &this->completed_invite_
                                                               : &this->completed_control_;
  const std::string call_id = header_value(request, "Call-ID");
  const uint32_t request_cseq = cseq_number(header_value(request, "CSeq"));
  const std::string request_branch = via_branch(header_value(request, "Via"));
  if (call_id.empty() || request_cseq == 0) return;
  if (method == "INVITE" && completed->awaiting_ack &&
      completed->call_id == this->call_id_ && this->last_invite_cseq_number_ != 0 &&
      (completed->call_id != call_id || completed->cseq != request_cseq ||
       completed->branch != request_branch)) {
    // Do not let a colliding/re-INVITE stateless error evict the active UAS
    // final response before its ACK arrives.
    return;
  }
  const uint32_t now = millis();
  completed->method = method;
  completed->call_id = call_id;
  completed->cseq = request_cseq;
  completed->branch = request_branch;
  completed->from_tag = tag_from_header(header_value(response, "From"));
  completed->to_tag = tag_from_header(header_value(response, "To"));
  completed->response = response;
  completed->peer_ip_v4 = peer_ip_v4;
  completed->peer_port = peer_port;
  completed->status = static_cast<uint16_t>(parsed_status);
  completed->completed_ms = now;
  completed->next_retransmit_ms = now + SIP_T1_MS;
  completed->deadline_ms = now + SIP_TRANSACTION_TIMEOUT_MS;
  completed->retransmit_interval_ms = SIP_T1_MS;
  completed->retransmits = 0;
  completed->udp = !this->remote_sip_tcp_.load(std::memory_order_acquire);
  completed->awaiting_ack = method == "INVITE" && completed->udp;
  if (completed->awaiting_ack) this->wake_sip_task_();
}

uint16_t SipTransport::acknowledge_completed_invite_(const std::string &request,
                                                     const sockaddr_in &src) {
  if (this->completed_invite_.empty()) return 0;
  const uint32_t now = millis();
  if (time_reached(now, this->completed_invite_.completed_ms + SIP_TRANSACTION_TIMEOUT_MS)) {
    this->completed_invite_.clear();
    return 0;
  }
  const std::string cseq = header_value(request, "CSeq");
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  const std::string branch = via_branch(header_value(request, "Via"));
  const bool transport_matches =
      this->completed_invite_.udp != this->remote_sip_tcp_.load(std::memory_order_acquire);
  const bool transaction_matches =
      cseq_method(cseq) == "ACK" && cseq_number(cseq) == this->completed_invite_.cseq &&
      header_value(request, "Call-ID") == this->completed_invite_.call_id &&
      peer_ip == this->completed_invite_.peer_ip_v4 && transport_matches &&
      tag_from_header(header_value(request, "From")) == this->completed_invite_.from_tag &&
      tag_from_header(header_value(request, "To")) == this->completed_invite_.to_tag &&
      (this->completed_invite_.status < 300 ||
       this->completed_invite_.branch.empty() || branch == this->completed_invite_.branch);
  if (!transaction_matches) return 0;
  this->completed_invite_.awaiting_ack = false;
  ESP_LOGI(TAG, "SIP ACK completed cached INVITE server transaction call_id=%s",
           this->completed_invite_.call_id.c_str());
  return this->completed_invite_.status;
}

bool SipTransport::replay_completed_invite_ack_(const std::string &response,
                                                const sockaddr_in &src) {
  if (this->completed_invite_client_.empty()) return false;
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) return false;
  if (millis() - this->completed_invite_client_.completed_ms > SIP_TRANSACTION_TIMEOUT_MS) {
    this->completed_invite_client_.clear();
    return false;
  }
  if (response.rfind("SIP/2.0 ", 0) != 0 || response.size() < 12) return false;
  const int status = std::atoi(response.substr(8, 3).c_str());
  const std::string cseq = header_value(response, "CSeq");
  const uint32_t peer_ip = ntohl(src.sin_addr.s_addr);
  if (status < 200 || cseq_method(cseq) != "INVITE" ||
      header_value(response, "Call-ID") != this->completed_invite_client_.call_id ||
      cseq_number(cseq) != this->completed_invite_client_.cseq ||
      peer_ip != this->completed_invite_client_.response_ip_v4 ||
      (!this->completed_invite_client_.branch.empty() &&
       via_branch(header_value(response, "Via")) != this->completed_invite_client_.branch)) {
    return false;
  }
  ESP_LOGI(TAG, "SIP INVITE final retransmission replaying ACK for call_id=%s",
           this->completed_invite_client_.call_id.c_str());
  this->send_sip_(this->completed_invite_client_.ack,
                  this->completed_invite_client_.ack_ip_v4,
                  this->completed_invite_client_.ack_port);
  return true;
}

void SipTransport::remember_completed_invite_ack_(const std::string &request,
                                                  uint32_t target_ip_v4,
                                                  uint16_t target_port) {
  if (request.empty() || target_ip_v4 == 0 || target_port == 0) return;
  this->completed_invite_client_.call_id = this->call_id_;
  this->completed_invite_client_.cseq = this->invite_cseq_;
  this->completed_invite_client_.branch = this->branch_;
  this->completed_invite_client_.ack = request;
  this->completed_invite_client_.response_ip_v4 =
      this->remote_ip_v4_.load(std::memory_order_acquire);
  this->completed_invite_client_.ack_ip_v4 = target_ip_v4;
  this->completed_invite_client_.ack_port = target_port;
  this->completed_invite_client_.completed_ms = millis();
}

bool SipTransport::handle_invite_(const std::string &message, const sockaddr_in &src) {
  const std::string body = message_body(message);
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const std::string incoming_call_id = header_value(message, "Call-ID");
  const std::string incoming_via = header_values(message, "Via");
  const std::string incoming_from = header_value(message, "From");
  const std::string incoming_to = header_value(message, "To");
  const std::string incoming_cseq = header_value(message, "CSeq");
  const uint32_t incoming_cseq_number = cseq_number(incoming_cseq);
  if (incoming_call_id.empty() || incoming_via.empty() || incoming_from.empty() || incoming_to.empty() ||
      incoming_cseq_number == 0 || cseq_method(incoming_cseq) != "INVITE" ||
      tag_from_header(incoming_from).empty()) {
    ESP_LOGW(TAG, "SIP INVITE rejected: missing or invalid transaction headers");
    return this->send_stateless_response_(message, src, 400, "Bad Request");
  }
  if (this->replay_completed_response_(message, src, "INVITE")) {
    return true;
  }
  std::string incoming_caller_name =
      sip_header_token(header_value(message, "X-Voip-Stack-Caller-Name"), VOIP_STACK_MAX_NAME_LEN);
  std::string incoming_dest_name =
      sip_header_token(header_value(message, "X-Voip-Stack-Dest-Name"), VOIP_STACK_MAX_NAME_LEN);
  if (incoming_caller_name.empty()) {
    incoming_caller_name = sip_header_token(sip_user_from_header(incoming_from), VOIP_STACK_MAX_NAME_LEN);
  }
  if (incoming_dest_name.empty()) {
    incoming_dest_name = sip_header_token(sip_user_from_header(incoming_to), VOIP_STACK_MAX_NAME_LEN);
  }
  if (!incoming_call_id.empty() && !this->call_id_.empty() && incoming_call_id != this->call_id_ &&
      this->last_invite_cseq_number_ == 0 &&
      !this->media_active_.load(std::memory_order_acquire) && !this->dialog_active_()) {
    // The FSM has already ended an outbound call, but its CANCEL/BYE client
    // transaction may still be waiting for a response. Prefer a real new
    // inbound call over keeping that terminal dialog as a false busy lock.
    ESP_LOGI(TAG, "SIP releasing terminal outbound dialog for new inbound INVITE");
    this->reset_dialog_();
  }
  if (!incoming_call_id.empty() && !this->call_id_.empty() && incoming_call_id != this->call_id_) {
    const uint32_t active_peer_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
    const bool glare = this->outgoing_invite_pending_.load(std::memory_order_acquire) &&
                       active_peer_ip == src_ip && !this->caller_name_.empty() &&
                       incoming_caller_name == this->dest_name_;
    if (!glare) {
      ESP_LOGW(TAG, "SIP INVITE rejected busy: active_call_id=%s incoming_call_id=%s",
               this->call_id_.c_str(), incoming_call_id.c_str());
      return this->send_stateless_response_(message, src, 486, "Busy Here", "busy", true);
    }
    const bool local_invite_wins = this->caller_name_ < incoming_caller_name ||
                                   (this->caller_name_ == incoming_caller_name &&
                                    this->call_id_ < incoming_call_id);
    if (local_invite_wins) {
      ESP_LOGW(TAG, "SIP glare with %s: retaining local INVITE", incoming_caller_name.c_str());
      return this->send_stateless_response_(message, src, 486, "Busy Here", "glare", true);
    }
    ESP_LOGW(TAG, "SIP glare with %s: cancelling local INVITE", incoming_caller_name.c_str());
    this->send_cancel_unlocked_(this->call_id_);
    this->reset_dialog_();
  }
  const uint32_t active_peer_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (!this->call_id_.empty() && incoming_call_id == this->call_id_ && active_peer_ip != 0 &&
      src_ip != active_peer_ip) {
    ESP_LOGW(TAG, "SIP INVITE rejected for active call_id=%s from unexpected peer",
             incoming_call_id.c_str());
    return this->send_stateless_response_(message, src, 481,
                                          "Call/Transaction Does Not Exist");
  }
  if (!incoming_call_id.empty() && incoming_call_id == this->call_id_ &&
      this->last_invite_cseq_number_ != 0 &&
      incoming_cseq_number != this->last_invite_cseq_number_) {
    ESP_LOGW(TAG, "SIP re-INVITE rejected: call_id=%s old_cseq=%u new_cseq=%u",
             incoming_call_id.c_str(),
             (unsigned) this->last_invite_cseq_number_,
             (unsigned) incoming_cseq_number);
    return this->send_stateless_response_(message, src, 488, "Not Acceptable Here", "reinvite_unsupported", true);
  }
  if (!incoming_call_id.empty() && incoming_call_id == this->call_id_ &&
      this->last_invite_cseq_number_ == incoming_cseq_number) {
    const std::string incoming_branch = via_branch(incoming_via);
    const std::string active_branch = via_branch(this->last_invite_via_);
    if (!active_branch.empty() && incoming_branch != active_branch) {
      ESP_LOGW(TAG, "SIP merged INVITE rejected for call_id=%s", incoming_call_id.c_str());
      return this->send_stateless_response_(message, src, 482, "Loop Detected");
    }
    if (!this->last_invite_response_.empty()) {
      ESP_LOGD(TAG, "SIP INVITE retransmission replaying latest provisional response");
      this->send_sip_(this->last_invite_response_, src_ip, ntohs(src.sin_port));
      return true;
    }
  }
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  this->call_id_ = incoming_call_id;
  this->last_invite_via_ = incoming_via;
  this->last_invite_from_ = incoming_from;
  this->last_invite_to_ = incoming_to;
  this->last_invite_cseq_ = incoming_cseq;
  this->last_invite_cseq_number_ = incoming_cseq_number;
  this->remote_tag_ = tag_from_header(this->last_invite_from_);
  if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
  const std::string remote_identity_uri = strip_angle_uri(this->last_invite_from_);
  this->remote_uri_ = remote_identity_uri.empty() ? "" : "<" + remote_identity_uri + ">";
  this->remote_target_uri_ = strip_angle_uri(header_value(message, "Contact"));
  if (this->remote_target_uri_.empty()) {
    this->remote_target_uri_ = strip_angle_uri(this->last_invite_from_);
  }
  this->local_uri_ = this->last_invite_to_;
  std::string local_contact_ip = "0.0.0.0";
  this->local_ip_for_peer_(src_ip, &local_contact_ip);
  const char *contact_transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "tcp" : "udp";
  const std::string local_contact_user = sip_uri_user_encode(sip_user_from_header(this->last_invite_to_));
  this->local_uri_ = "<sip:" + local_contact_user + "@" + local_contact_ip + ":" +
                     std::to_string(this->sip_port_) + ";transport=" + contact_transport + ">";
  if (!this->learn_remote_rtp_from_sdp_(body, src_ip)) {
    const bool sent = this->send_response_(488, "Not Acceptable Here");
    this->reset_dialog_();
    return sent;
  }
  this->send_response_(100, "Trying");

  std::string from_user = incoming_caller_name;
  std::string to_user = incoming_dest_name;
  if (from_user.empty() || to_user.empty()) {
    const bool sent = this->send_response_(400, "Bad Request");
    this->reset_dialog_();
    return sent;
  }
  this->caller_name_ = from_user;
  this->dest_name_ = to_user;
  this->caller_route_ = sip_header_token(header_value(message, "X-Voip-Stack-Caller-Route"),
                                         VOIP_STACK_MAX_ROUTE_ID_LEN);
  this->dest_route_ = sip_header_token(header_value(message, "X-Voip-Stack-Dest-Route"),
                                       VOIP_STACK_MAX_ROUTE_ID_LEN);
  if (this->caller_route_.empty()) this->caller_route_ = this->caller_name_;
  if (this->dest_route_.empty()) this->dest_route_ = this->dest_name_;

  ESP_LOGI(TAG, "SIP INVITE accepted into FSM call_id=%s", this->call_id_.c_str());
  AudioFormat selected_tx;
  AudioFormat selected_rx;
  this->get_media_config_(&selected_tx, &selected_rx, nullptr, nullptr);
  SipSignal signal;
  signal.type = SipSignalType::INVITE;
  signal.call_id = this->call_id_;
  signal.caller_route = this->caller_route_;
  signal.caller_name = this->caller_name_;
  signal.dest_route = this->dest_route_;
  signal.dest_name = this->dest_name_;
  signal.caller_tx_formats.formats[0] = selected_rx;
  signal.caller_tx_formats.count = 1;
  signal.caller_rx_formats.formats[0] = selected_tx;
  signal.caller_rx_formats.count = 1;
  signal.selected_rx_format = selected_rx;
  signal.selected_tx_format = selected_tx;
  this->emit_sip_signal_(signal);
  return true;
}

bool SipTransport::handle_response_(const std::string &message, const sockaddr_in &src) {
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  if (message.rfind("SIP/2.0 ", 0) != 0 || message.size() < 12) return false;
  if (!std::isdigit(static_cast<unsigned char>(message[8])) ||
      !std::isdigit(static_cast<unsigned char>(message[9])) ||
      !std::isdigit(static_cast<unsigned char>(message[10]))) {
    return false;
  }
  const int status = std::atoi(message.substr(8, 3).c_str());
  if (status < 100 || status > 699) return false;
  if (this->replay_completed_invite_ack_(message, src)) return true;
  const std::string response_call_id = header_value(message, "Call-ID");
  if (response_call_id.empty() || this->call_id_.empty() || response_call_id != this->call_id_) {
    ESP_LOGD(TAG, "SIP response ignored for stale/unknown call_id=%s current=%s",
             response_call_id.empty() ? "(empty)" : response_call_id.c_str(),
             this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
    return true;
  }
  const std::string response_cseq = header_value(message, "CSeq");
  const std::string method = cseq_method(response_cseq);
  const uint32_t response_cseq_number = cseq_number(response_cseq);
  if (method.empty() || response_cseq_number == 0) {
    ESP_LOGD(TAG, "SIP response ignored without a valid CSeq");
    return true;
  }
  if ((method == "INVITE" || method == "CANCEL") && response_cseq_number != this->invite_cseq_) {
    ESP_LOGD(TAG, "SIP response ignored for stale CSeq %u %s (active INVITE CSeq=%u)",
             (unsigned) response_cseq_number, method.c_str(), (unsigned) this->invite_cseq_);
    return true;
  }
  if (method != "INVITE" && method != "CANCEL" && method != "BYE") {
    ESP_LOGD(TAG, "SIP response ignored for unsupported transaction method %s", method.c_str());
    return true;
  }
  const std::string response_from_tag = tag_from_header(header_value(message, "From"));
  if (this->local_tag_.empty() || response_from_tag != this->local_tag_) {
    ESP_LOGD(TAG, "SIP response ignored for mismatched local From tag");
    return true;
  }
  if (method == "BYE" && !this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    if (this->pending_bye_.empty() ||
        response_cseq_number != cseq_number(header_value(this->pending_bye_.request, "CSeq")) ||
        via_branch(header_value(message, "Via")) != via_branch(header_value(this->pending_bye_.request, "Via"))) {
      ESP_LOGD(TAG, "SIP response ignored for mismatched BYE transaction");
      return true;
    }
  }
  if (method == "INVITE" || method == "CANCEL") {
    const std::string response_branch = via_branch(header_value(message, "Via"));
    if (this->branch_.empty() || response_branch.empty() || response_branch != this->branch_) {
      ESP_LOGD(TAG, "SIP response ignored for mismatched %s transaction branch", method.c_str());
      return true;
    }
  }
  if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
    uint32_t expected_response_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
    const UdpTransaction *transaction = method == "BYE" ? &this->pending_bye_
                                        : method == "CANCEL" ? &this->pending_cancel_
                                                              : &this->pending_invite_;
    if (!transaction->empty() && transaction->ip_v4 != 0) expected_response_ip = transaction->ip_v4;
    if (expected_response_ip != 0 && src_ip != expected_response_ip) {
      ESP_LOGD(TAG, "SIP response ignored for %s from unexpected peer", method.c_str());
      return true;
    }
  }
  const std::string response_to_tag = tag_from_header(header_value(message, "To"));
  if (status > 100) {
    if (!response_to_tag.empty() && !this->remote_tag_.empty() && response_to_tag != this->remote_tag_) {
      ESP_LOGD(TAG, "SIP response ignored for missing/mismatched remote To tag");
      return true;
    }
    if (response_to_tag.empty() && method != "INVITE") {
      ESP_LOGD(TAG, "SIP response ignored without a remote To tag");
      return true;
    }
    if (this->remote_tag_.empty() && !response_to_tag.empty()) this->remote_tag_ = response_to_tag;
  }
  // Only a response belonging to the active dialog may retarget subsequent
  // ACK/BYE traffic. Stale or spoofed responses must be side-effect free.
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  this->mark_sip_event_(SipEvent::RESPONSE, static_cast<uint16_t>(status));
  if (method == "INVITE" && status >= 100) {
    if (status < 200 && !this->pending_invite_.empty()) {
      // A provisional response stops INVITE retransmission but not Timer B.
      // Keep the bounded transaction until its original 64*T1 deadline so a
      // peer that sends 100/180 and then disappears cannot strand the call.
      this->pending_invite_.completed = true;
      this->pending_invite_.next_ms = this->pending_invite_.deadline_ms;
    } else {
      this->clear_invite_transaction_();
    }
  }
  if (status > 100 && status < 200 && method == "INVITE") {
    SipSignal signal;
    signal.type = SipSignalType::STATUS_180_RINGING;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    this->emit_sip_signal_(signal);
    return true;
  }
  if (status >= 200 && status < 300) {
    if (method == "BYE") {
      ESP_LOGI(TAG, "SIP BYE completed call_id=%s", this->call_id_.c_str());
      this->clear_bye_transaction_();
      this->reset_dialog_();
      return true;
    }
    if (method == "CANCEL") {
      ESP_LOGI(TAG, "SIP CANCEL completed call_id=%s", this->call_id_.c_str());
      if (!this->pending_cancel_.empty()) {
        // Stop retransmitting CANCEL, but retain the transaction until Timer F
        // while waiting for the INVITE's final 487.
        this->pending_cancel_.completed = true;
        this->pending_cancel_.next_ms = this->pending_cancel_.deadline_ms;
      }
      return true;
    }
    if (method != "INVITE") {
      ESP_LOGI(TAG, "SIP %u response for %s ignored", status, method.c_str());
      return true;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    const std::string contact_target = strip_angle_uri(header_value(message, "Contact"));
    if (!contact_target.empty()) this->remote_target_uri_ = contact_target;
    SipRequestOptions options;
    options.cseq_number = this->invite_cseq_;
    this->send_request_("ACK", "", options);
    if (this->remote_tag_.empty()) {
      SipSignal signal;
      signal.type = SipSignalType::PROTOCOL_ERROR;
      signal.status_code = 500;
      signal.call_id = this->call_id_;
      signal.reason = "malformed_2xx";
      this->reset_dialog_();
      this->emit_sip_signal_(signal);
      return true;
    }
    const bool media_ok = this->learn_remote_rtp_from_sdp_(message_body(message), src_ip);
    if (this->cancel_requested_.load(std::memory_order_acquire)) {
      // CANCEL crossed the final 2xx. The dialog is confirmed despite the
      // cancellation, so ACK it and immediately terminate it with BYE.
      if (!this->send_request_("BYE")) {
        this->reset_dialog_();
      }
      return true;
    }
    if (!media_ok) {
      // A 2xx INVITE response still requires ACK. Terminate the confirmed SIP
      // dialog immediately instead of opening RTP with stale/default media.
      const bool bye_pending = this->send_request_("BYE");
      SipSignal signal;
      signal.type = SipSignalType::MEDIA_INCOMPATIBLE;
      signal.status_code = 488;
      signal.call_id = this->call_id_;
      signal.reason = "media_incompatible";
      if (!bye_pending) this->reset_dialog_();
      this->emit_sip_signal_(signal);
      return true;
    }
    this->open_media_session_();
    SipSignal signal;
    signal.type = SipSignalType::STATUS_200_OK;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    this->get_media_config_(&signal.selected_tx_format, &signal.selected_rx_format, nullptr, nullptr);
    this->emit_sip_signal_(signal);
    return true;
  }
  if (status >= 300) {
    if (method != "INVITE") {
      ESP_LOGW(TAG, "SIP %u response for %s", status, method.c_str());
      if (method == "BYE") {
        this->clear_bye_transaction_();
        this->reset_dialog_();
      } else if (method == "CANCEL") {
        if (!this->pending_cancel_.empty()) {
          this->pending_cancel_.completed = true;
          this->pending_cancel_.next_ms = this->pending_cancel_.deadline_ms;
        }
      }
      return true;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    this->send_invite_error_ack_();
    std::string reason = sip_header_token(header_value(message, "X-Voip-Stack-Decline-Reason"));
    if (reason.empty()) reason = reason_text_from_header(header_value(message, "Reason"));
    if (reason.empty()) reason = sip_failure_reason_(status);
    SipSignal signal;
    signal.type = status == 401 ? SipSignalType::AUTH_REQUIRED
                : status == 407 ? SipSignalType::PROXY_AUTH_REQUIRED
                : status == 488 ? SipSignalType::MEDIA_INCOMPATIBLE
                                : SipSignalType::FINAL_RESPONSE;
    signal.status_code = static_cast<uint16_t>(status);
    signal.call_id = this->call_id_;
    signal.reason = reason;
    this->emit_sip_signal_(signal);
    this->reset_dialog_();
    return true;
  }
  return true;
}

void SipTransport::handle_sip_datagram_(const char *data, size_t len, const sockaddr_in &src) {
  LockGuard lock(this->dialog_mutex_);
  const std::string msg(data, len);
  size_t declared_body_len = 0;
  const std::string content_length = header_value(msg, "Content-Length");
  const size_t body_separator = msg.find("\r\n\r\n");
  const bool invalid_framing = !sip_content_length(msg, &declared_body_len) ||
                               (!content_length.empty() &&
                                (body_separator == std::string::npos ||
                                 declared_body_len != msg.size() - body_separator - 4));
  if (invalid_framing) {
    ESP_LOGW(TAG, "SIP message rejected: invalid Content-Length framing");
    if (msg.rfind("SIP/2.0 ", 0) != 0) {
      this->send_stateless_response_(msg, src, 400, "Bad Request");
    }
    return;
  }
  if (msg.rfind("SIP/2.0 ", 0) == 0) {
    this->handle_response_(msg, src);
    return;
  }
  const size_t first_space = msg.find(' ');
  const std::string method = first_space == std::string::npos ? "" : msg.substr(0, first_space);
  ESP_LOGI(TAG, "SIP RX method=%s len=%u", method.c_str(), (unsigned) len);
  if ((method == "CANCEL" || method == "BYE") &&
      this->replay_completed_response_(msg, src, method)) {
    return;
  }
  const SipEvent event = sip_event_from_method_(method);
  if (event != SipEvent::NONE) this->mark_sip_event_(event);
  if (method == "INVITE") {
    this->handle_invite_(msg, src);
  } else if (method == "ACK") {
    const uint16_t completed_status = this->acknowledge_completed_invite_(msg, src);
    if (completed_status >= 300) return;
    const std::string request_call_id = header_value(msg, "Call-ID");
    const uint32_t expected_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
    const uint32_t request_cseq = cseq_number(header_value(msg, "CSeq"));
    const bool valid_ack = completed_status >= 200 && completed_status < 300 &&
                           !request_call_id.empty() && !this->call_id_.empty() &&
                           request_call_id == this->call_id_ &&
                           (expected_ip == 0 || ntohl(src.sin_addr.s_addr) == expected_ip) &&
                           this->last_invite_cseq_number_ != 0 &&
                           request_cseq == this->last_invite_cseq_number_ &&
                           cseq_method(header_value(msg, "CSeq")) == "ACK" &&
                           tag_from_header(header_value(msg, "From")) == this->remote_tag_ &&
                           tag_from_header(header_value(msg, "To")) == this->local_tag_;
    if (!valid_ack) {
      ESP_LOGD(TAG, "SIP ACK ignored for stale/invalid call_id=%s current=%s",
               request_call_id.empty() ? "(empty)" : request_call_id.c_str(),
               this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
      return;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    this->open_media_session_();
  } else if (method == "BYE") {
    if (this->reject_if_stale_dialog_(msg, src, "BYE")) return;
    if (!this->media_active_.load(std::memory_order_acquire)) {
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    this->send_stateless_response_(msg, src, 200, "OK", "", true);
    SipSignal signal;
    signal.type = SipSignalType::BYE;
    signal.call_id = this->call_id_;
    this->emit_sip_signal_(signal);
    this->reset_dialog_();
  } else if (method == "CANCEL") {
    if (this->reject_if_stale_dialog_(msg, src, "CANCEL")) return;
    const uint32_t incoming_cseq_number = cseq_number(header_value(msg, "CSeq"));
    const std::string incoming_via = header_value(msg, "Via");
    const std::string incoming_branch = via_branch(incoming_via);
    const std::string invite_branch = via_branch(this->last_invite_via_);
    const std::string incoming_from_tag = tag_from_header(header_value(msg, "From"));
    const bool same_transaction_via = !incoming_branch.empty() && !invite_branch.empty()
                                          ? incoming_branch == invite_branch
                                          : incoming_via == this->last_invite_via_;
    if (cseq_method(header_value(msg, "CSeq")) != "CANCEL" ||
        this->media_active_.load(std::memory_order_acquire) || this->last_invite_cseq_number_ == 0 ||
        incoming_cseq_number != this->last_invite_cseq_number_ || !same_transaction_via) {
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    if (incoming_from_tag.empty() || incoming_from_tag != this->remote_tag_) {
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    this->send_stateless_response_(msg, src, 200, "OK", "", true);
    this->send_response_(487, "Request Terminated");
    SipSignal signal;
    signal.type = SipSignalType::CANCEL;
    signal.status_code = 487;
    signal.call_id = this->call_id_;
    signal.reason = "cancelled";
    this->emit_sip_signal_(signal);
    this->reset_dialog_();
  } else if (method == "OPTIONS") {
    this->send_stateless_response_(msg, src, 200, "OK");
  } else if (sip_method_known_(method)) {
    this->send_stateless_response_(msg, src, 405, "Method Not Allowed");
  } else {
    this->send_stateless_response_(msg, src, 501, "Not Implemented");
  }
}

bool SipTransport::reject_if_stale_dialog_(const std::string &request, const sockaddr_in &src,
                                           const char *method_name) {
  const std::string request_call_id = header_value(request, "Call-ID");
  const bool call_id_matches =
      !request_call_id.empty() && !this->call_id_.empty() && request_call_id == this->call_id_;
  const uint32_t expected_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const bool host_matches = expected_ip == 0 || ntohl(src.sin_addr.s_addr) == expected_ip;
  bool dialog_tags_match = true;
  if (std::strcmp(method_name, "BYE") == 0) {
    const std::string from_tag = tag_from_header(header_value(request, "From"));
    const std::string to_tag = tag_from_header(header_value(request, "To"));
    dialog_tags_match = !this->remote_tag_.empty() && !this->local_tag_.empty() &&
                        from_tag == this->remote_tag_ && to_tag == this->local_tag_ &&
                        cseq_method(header_value(request, "CSeq")) == "BYE" &&
                        cseq_number(header_value(request, "CSeq")) > this->last_invite_cseq_number_;
  }
  if (call_id_matches && host_matches && dialog_tags_match) return false;
  ESP_LOGW(TAG, "SIP %s ignored for stale call_id=%s current=%s",
           method_name, request_call_id.c_str(), this->call_id_.c_str());
  this->send_stateless_response_(request, src, 481, "Call/Transaction Does Not Exist");
  return true;
}

void SipTransport::handle_sip_stream_(int socket, const sockaddr_in &src) {
  char buf[1024];
  auto drop_tcp_stream = [&](const char *reason) {
    char ip[16];
    inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
    ESP_LOGW(TAG, "%s from %s, dropping connection", reason, ip);
    this->handle_tcp_peer_loss_();
  };
  while (true) {
    const int n = recv(socket, buf, sizeof(buf), 0);
    if (n > 0) {
      this->sip_tcp_rx_buffer_.append(buf, static_cast<size_t>(n));
      if (this->sip_tcp_rx_buffer_.size() > MAX_SIP_TCP_RX_BUFFER) {
        drop_tcp_stream("SIP TCP RX buffer overflow");
        return;
      }
      continue;
    }
    if (n == 0) {
      ESP_LOGI(TAG, "SIP TCP peer closed");
      this->handle_tcp_peer_loss_();
      return;
    }
    const int err = errno;
    if (err == EWOULDBLOCK || err == EAGAIN || err == ENOTCONN || err == EINPROGRESS || err == EALREADY) break;
    ESP_LOGW(TAG, "SIP TCP RX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
    this->handle_tcp_peer_loss_();
    return;
  }

  while (true) {
    const size_t sep = this->sip_tcp_rx_buffer_.find("\r\n\r\n");
    if (sep == std::string::npos) return;
    size_t body_len = 0;
    if (!sip_content_length(this->sip_tcp_rx_buffer_, &body_len)) {
      drop_tcp_stream("SIP TCP invalid or ambiguous Content-Length");
      return;
    }
    if (body_len > MAX_SIP_BODY_BYTES) {
      drop_tcp_stream("SIP TCP Content-Length exceeds limit");
      return;
    }
    const size_t total = sep + 4 + body_len;
    if (total > MAX_SIP_TCP_RX_BUFFER) {
      drop_tcp_stream("SIP TCP framed message exceeds RX buffer");
      return;
    }
    if (this->sip_tcp_rx_buffer_.size() < total) return;
    const std::string msg = this->sip_tcp_rx_buffer_.substr(0, total);
    this->sip_tcp_rx_buffer_.erase(0, total);
    this->remote_sip_tcp_.store(true, std::memory_order_release);
    this->handle_sip_datagram_(msg.data(), msg.size(), src);
  }
}

void SipTransport::sip_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->sip_task_();
}

void SipTransport::rtp_task_trampoline_(void *param) {
  static_cast<SipTransport *>(param)->rtp_task_();
}

void SipTransport::sip_task_() {
  uint8_t buf[2048];
  int connecting_fd = -1;
  uint32_t connect_deadline_ms = 0;
  uint32_t connecting_ip_v4 = 0;
  uint16_t connecting_port = 0;
  auto close_connecting = [&]() {
    if (connecting_fd >= 0) close(connecting_fd);
    connecting_fd = -1;
    connect_deadline_ms = 0;
    connecting_ip_v4 = 0;
    connecting_port = 0;
  };
  auto drop_tcp_pending = [this]() {
    LockGuard lock(this->tcp_tx_pending_mutex_);
    this->tcp_tx_pending_.clear();
  };
  auto fail_tcp_connect = [&](int err) {
    char ip[16];
    struct in_addr a{};
    a.s_addr = htonl(connecting_ip_v4);
    inet_ntoa_r(a, ip, sizeof(ip));
    ESP_LOGW(TAG, "SIP TCP connect to %s:%u failed: %s (%d: %s)",
             ip, (unsigned) connecting_port, socket_errno_name(err), err, socket_errno_text(err));
    close_connecting();
    this->tcp_connect_requested_.store(false, std::memory_order_release);
    drop_tcp_pending();
    {
      LockGuard lock(this->dialog_mutex_);
      if (!this->call_id_.empty()) this->reset_dialog_();
    }
    this->emit_connection_change_(false);
  };
  auto promote_tcp_connect = [&]() {
    const int promoted_fd = connecting_fd;
    const uint32_t promoted_ip = connecting_ip_v4;
    const uint16_t promoted_port = connecting_port;
    std::string pending;
    bool pending_sent = true;
    {
      LockGuard send_lock(this->tcp_send_mutex_);
      this->sip_tcp_client_socket_.store(promoted_fd, std::memory_order_release);
      this->sip_tcp_client_ip_v4_.store(promoted_ip, std::memory_order_release);
      this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
      this->tcp_connect_requested_.store(false, std::memory_order_release);
      this->sip_tcp_rx_buffer_.clear();
      {
        LockGuard lock(this->tcp_tx_pending_mutex_);
        pending.swap(this->tcp_tx_pending_);
      }
      if (!pending.empty()) {
        pending_sent = this->send_sip_tcp_record_(pending, promoted_fd);
      }
    }
    char ip[16];
    struct in_addr a{};
    a.s_addr = htonl(promoted_ip);
    inet_ntoa_r(a, ip, sizeof(ip));
    ESP_LOGI(TAG, "SIP TCP originate connected to %s:%u", ip, (unsigned) promoted_port);
    connecting_fd = -1;
    connect_deadline_ms = 0;
    connecting_ip_v4 = 0;
    connecting_port = 0;
    if (!pending_sent) this->handle_tcp_peer_loss_();
  };
  while (this->running_.load(std::memory_order_acquire)) {
    this->pump_udp_retransmits_();
    if (this->sip_tcp_client_close_requested_.load(std::memory_order_acquire)) {
      close_connecting();
      this->close_tcp_client_from_sip_task_();
    }
    if (this->tcp_connect_requested_.load(std::memory_order_acquire)) {
      close_connecting();
      this->close_tcp_client_from_sip_task_();
      connecting_ip_v4 = this->tcp_connect_ip_v4_.load(std::memory_order_acquire);
      connecting_port = this->tcp_connect_port_.load(std::memory_order_acquire);
      if (connecting_ip_v4 == 0 || connecting_port == 0) {
        this->tcp_connect_requested_.store(false, std::memory_order_release);
        drop_tcp_pending();
        {
          LockGuard lock(this->dialog_mutex_);
          if (!this->call_id_.empty()) this->reset_dialog_();
        }
        this->emit_connection_change_(false);
      } else {
        connecting_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connecting_fd < 0) {
          fail_tcp_connect(errno);
        } else {
          int opt = 1;
          setsockopt(connecting_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
          int flags = fcntl(connecting_fd, F_GETFL, 0);
          fcntl(connecting_fd, F_SETFL, flags | O_NONBLOCK);

          struct sockaddr_in dest{};
          dest.sin_family = AF_INET;
          dest.sin_addr.s_addr = htonl(connecting_ip_v4);
          dest.sin_port = htons(connecting_port);
          const int rc = connect(connecting_fd, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
          if (rc == 0) {
            promote_tcp_connect();
          } else if (errno == EINPROGRESS) {
            connect_deadline_ms = millis() + 2000;
            this->tcp_connect_requested_.store(false, std::memory_order_release);
          } else {
            fail_tcp_connect(errno);
          }
        }
      }
    }
    fd_set readfds;
    fd_set writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    int max_fd = -1;
    if (this->sip_socket_ >= 0) {
      FD_SET(this->sip_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_socket_);
    }
    if (this->sip_tcp_listener_socket_ >= 0) {
      FD_SET(this->sip_tcp_listener_socket_, &readfds);
      max_fd = std::max(max_fd, this->sip_tcp_listener_socket_);
    }
    const int tcp_client = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
    if (tcp_client >= 0) {
      FD_SET(tcp_client, &readfds);
      max_fd = std::max(max_fd, tcp_client);
    }
    if (connecting_fd >= 0) {
      FD_SET(connecting_fd, &writefds);
      max_fd = std::max(max_fd, connecting_fd);
    }
    if (max_fd < 0) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    struct timeval timeout{};
    struct timeval *timeout_ptr = nullptr;
    uint32_t timeout_ms = 0;
    bool have_timeout = false;
    if (connecting_fd >= 0) {
      const uint32_t now = millis();
      timeout_ms = time_reached(now, connect_deadline_ms) ? 0 : connect_deadline_ms - now;
      have_timeout = true;
      timeout_ptr = &timeout;
    }
    if (!this->remote_sip_tcp_.load(std::memory_order_acquire)) {
      const uint32_t now = millis();
      uint32_t next_ms = 0;
      bool have_udp_timeout = false;
      auto include_at = [now, &next_ms, &have_udp_timeout](uint32_t deadline) {
        const uint32_t delta = time_reached(now, deadline) ? 0 : deadline - now;
        if (!have_udp_timeout || delta < next_ms) {
          next_ms = delta;
          have_udp_timeout = true;
        }
      };
      {
        LockGuard lock(this->dialog_mutex_);
        auto include_txn = [&include_at](const UdpTransaction &txn) {
          if (!txn.empty()) include_at(txn.next_ms);
        };
        if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
          include_txn(this->pending_invite_);
        }
        include_txn(this->pending_cancel_);
        include_txn(this->pending_bye_);
        if (this->completed_invite_.udp && this->completed_invite_.awaiting_ack) {
          include_at(this->completed_invite_.next_retransmit_ms);
        }
      }
      if (have_udp_timeout && (!have_timeout || next_ms < timeout_ms)) {
        timeout_ms = next_ms;
        have_timeout = true;
        timeout_ptr = &timeout;
      }
    }
    if (timeout_ptr != nullptr) {
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_usec = (timeout_ms % 1000) * 1000;
    }
    const int ready = select(max_fd + 1, &readfds, &writefds, nullptr, timeout_ptr);
    if (connecting_fd >= 0 && time_reached(millis(), connect_deadline_ms)) {
      fail_tcp_connect(ETIMEDOUT);
      continue;
    }
    if (ready <= 0) {
      continue;
    }

    if (connecting_fd >= 0 && FD_ISSET(connecting_fd, &writefds)) {
      int so_error = 0;
      socklen_t len = sizeof(so_error);
      if (getsockopt(connecting_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
        promote_tcp_connect();
      } else {
        fail_tcp_connect(so_error == 0 ? errno : so_error);
      }
    }

    if (this->sip_tcp_listener_socket_ >= 0 && FD_ISSET(this->sip_tcp_listener_socket_, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int client = accept(this->sip_tcp_listener_socket_, reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (client >= 0) {
        int opt = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        int flags = fcntl(client, F_GETFL, 0);
        fcntl(client, F_SETFL, flags | O_NONBLOCK);
        const uint32_t accepted_ip_v4 = ntohl(src.sin_addr.s_addr);
        const int active_client = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
        const uint32_t active_ip_v4 = this->sip_tcp_client_ip_v4_.load(std::memory_order_acquire);
        if (this->dialog_active_() && active_client >= 0 && active_ip_v4 != 0 && active_ip_v4 != accepted_ip_v4) {
          char ip[16];
          inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
          ESP_LOGW(TAG, "SIP TCP accept rejected: dialog active with different peer %s", ip);
          close(client);
          continue;
        }
        if (!this->should_accept_session_() && active_client < 0) {
          close(client);
          continue;
        }
        this->close_tcp_client_from_sip_task_();
        this->sip_tcp_client_socket_.store(client, std::memory_order_release);
        this->sip_tcp_client_ip_v4_.store(accepted_ip_v4, std::memory_order_release);
        this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
        this->sip_tcp_rx_buffer_.clear();
        this->remote_sip_tcp_.store(true, std::memory_order_release);
        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "SIP TCP accepted from %s:%u", ip, (unsigned) ntohs(src.sin_port));
      }
    }

    if (this->sip_socket_ >= 0 && FD_ISSET(this->sip_socket_, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int n = recvfrom(this->sip_socket_, buf, sizeof(buf) - 1, 0,
                       reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (n > 0) {
        const bool tcp_call_active =
            this->outgoing_invite_pending_.load(std::memory_order_acquire) ||
            this->media_active_.load(std::memory_order_acquire) || this->dialog_active_();
        const bool tcp_session_active =
            tcp_call_active && this->remote_sip_tcp_.load(std::memory_order_acquire) &&
            (this->sip_tcp_client_socket_.load(std::memory_order_acquire) >= 0 || connecting_fd >= 0 ||
             this->tcp_connect_requested_.load(std::memory_order_acquire));
        if (tcp_session_active) {
          // Both listeners remain open, but a stray UDP packet must never flip
          // an established TCP dialog to UDP or redirect its ACK/BYE traffic.
          ESP_LOGD(TAG, "SIP UDP datagram ignored while TCP signaling is active");
          continue;
        }
        if (this->sip_tcp_client_socket_.load(std::memory_order_acquire) >= 0) {
          // An idle keep-alive connection must not coexist with a new UDP
          // dialog, otherwise later TCP traffic could retarget the call.
          this->close_tcp_client_from_sip_task_();
        }
        this->remote_sip_tcp_.store(false, std::memory_order_release);
        buf[n] = 0;
        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "SIP UDP RX %d bytes from %s:%u", n, ip, (unsigned) ntohs(src.sin_port));
        this->handle_sip_datagram_(reinterpret_cast<const char *>(buf), static_cast<size_t>(n), src);
      }
    }

    const int active_tcp_client = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
    if (active_tcp_client >= 0 && FD_ISSET(active_tcp_client, &readfds)) {
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      getpeername(active_tcp_client, reinterpret_cast<struct sockaddr *>(&src), &slen);
      this->handle_sip_stream_(active_tcp_client, src);
    }
  }
  close_connecting();
  this->close_tcp_client_from_sip_task_();
  if (this->sip_task_done_ != nullptr) {
    xSemaphoreGive(this->sip_task_done_);
  }
  vTaskDelete(nullptr);
}

void SipTransport::rtp_task_() {
  uint8_t buf[1600];
  // The wire payload is capped at 1488 bytes. L24-in-S32 expands by 4/3
  // while decoding, so 2 KiB covers every format accepted by the schema.
  uint8_t pcm[2048];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (this->rtp_task_terminate_.load(std::memory_order_acquire)) {
      break;
    }
    while (this->rtp_running_.load(std::memory_order_acquire)) {
      const int socket = this->rtp_socket_;
      if (socket < 0) {
        break;
      }
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(socket, &readfds);
      const int ready = select(socket + 1, &readfds, nullptr, nullptr, nullptr);
      if (ready < 0) {
        if (errno != EINTR) {
          const int err = errno;
          ESP_LOGW(TAG, "RTP select failed: %s (%d: %s)",
                   socket_errno_name(err), err, socket_errno_text(err));
          break;
        }
        continue;
      }
      if (ready == 0 || !FD_ISSET(socket, &readfds)) {
        continue;
      }
      struct sockaddr_in src{};
      socklen_t slen = sizeof(src);
      int n = recvfrom(socket, buf, sizeof(buf), 0,
                       reinterpret_cast<struct sockaddr *>(&src), &slen);
      if (n <= 12 || (buf[0] & 0xC0) != 0x80) {
        continue;
      }
      if (!this->media_active_.load(std::memory_order_acquire)) {
        continue;
      }
      const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
      const uint16_t src_port = ntohs(src.sin_port);
      const uint32_t expected_ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
      if (expected_ip != 0 && src_ip != expected_ip) {
        continue;
      }
      const uint8_t csrc_count = buf[0] & 0x0F;
      size_t header = 12u + static_cast<size_t>(csrc_count) * 4u;
      if (static_cast<size_t>(n) <= header) {
        continue;
      }
      if ((buf[0] & 0x10) != 0) {
        if (static_cast<size_t>(n) < header + 4) continue;
        const uint16_t ext_len = static_cast<uint16_t>((buf[header + 2] << 8) | buf[header + 3]);
        header += 4u + static_cast<size_t>(ext_len) * 4u;
        if (static_cast<size_t>(n) <= header) continue;
      }
      size_t payload_len = static_cast<size_t>(n) - header;
      if ((buf[0] & 0x20) != 0 && payload_len > 0) {
        const uint8_t pad = buf[n - 1];
        if (pad == 0 || pad > payload_len) continue;
        payload_len -= pad;
      }
      AudioFormat rx_format;
      uint8_t rx_payload_type = 96;
      this->get_media_config_(nullptr, &rx_format, nullptr, &rx_payload_type);
      if ((buf[1] & 0x7F) != rx_payload_type) continue;
      const uint16_t sequence = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
      const uint32_t timestamp = (static_cast<uint32_t>(buf[4]) << 24) |
                                 (static_cast<uint32_t>(buf[5]) << 16) |
                                 (static_cast<uint32_t>(buf[6]) << 8) |
                                 static_cast<uint32_t>(buf[7]);
      const uint8_t *payload = buf + header;
      const uint32_t ssrc = (static_cast<uint32_t>(buf[8]) << 24) |
                            (static_cast<uint32_t>(buf[9]) << 16) |
                            (static_cast<uint32_t>(buf[10]) << 8) |
                            static_cast<uint32_t>(buf[11]);
      if (this->rtp_ssrc_latched_.load(std::memory_order_acquire) &&
          (this->latched_rtp_ip_v4_.load(std::memory_order_acquire) != src_ip ||
           this->latched_rtp_ssrc_.load(std::memory_order_acquire) != ssrc)) {
        continue;
      }
      const size_t out_len = rtp_payload_to_pcm(payload, payload_len, rx_format, pcm, sizeof(pcm));
      if (out_len == 0 || out_len != rx_format.nominal_frame_bytes()) continue;
      if (!this->rtp_ssrc_latched_.load(std::memory_order_acquire)) {
        this->latched_rtp_ip_v4_.store(src_ip, std::memory_order_release);
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->latched_rtp_ssrc_.store(ssrc, std::memory_order_release);
        this->remote_rtp_port_.store(src_port, std::memory_order_release);
        this->rtp_ssrc_latched_.store(true, std::memory_order_release);
      } else if (this->latched_rtp_port_.load(std::memory_order_acquire) != src_port) {
        // Keep the SSRC identity but follow a legitimate NAT port rebind.
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->remote_rtp_port_.store(src_port, std::memory_order_release);
      }
      this->rtp_rx_packets_.fetch_add(1, std::memory_order_acq_rel);
      this->rtp_rx_bytes_.fetch_add(static_cast<uint32_t>(n), std::memory_order_acq_rel);
      this->emit_audio_frame_(pcm, out_len, sequence, timestamp);
    }
    this->rtp_task_quiesced_.store(true, std::memory_order_release);
    if (this->rtp_task_done_ != nullptr) {
      xSemaphoreGive(this->rtp_task_done_);
    }
  }
  if (this->rtp_task_done_ != nullptr) {
    xSemaphoreGive(this->rtp_task_done_);
  }
  vTaskDelete(nullptr);
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_SIP_TRANSPORT
