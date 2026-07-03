#include "sip_transport.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <cstdio>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include <esp_system.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "audio_core_task_utils.h"
#include "net_utils.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.sip";

namespace {

std::string sip_header_token(const std::string &raw);

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
  const std::string needle = std::string(name) + ":";
  size_t pos = 0;
  while (pos < msg.size()) {
    const size_t end = msg.find("\r\n", pos);
    const size_t line_end = end == std::string::npos ? msg.size() : end;
    const std::string line = msg.substr(pos, line_end - pos);
    if (line.size() >= needle.size()) {
      bool match = true;
      for (size_t i = 0; i < needle.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(line[i])) !=
            std::tolower(static_cast<unsigned char>(needle[i]))) {
          match = false;
          break;
        }
      }
      if (match) return trim_copy(line.substr(needle.size()));
    }
    if (end == std::string::npos) break;
    pos = end + 2;
  }
  return "";
}

std::string message_body(const std::string &msg) {
  const size_t sep = msg.find("\r\n\r\n");
  if (sep == std::string::npos) return "";
  return msg.substr(sep + 4);
}

size_t sip_content_length(const std::string &msg) {
  const size_t sep = msg.find("\r\n\r\n");
  const size_t header_end = sep == std::string::npos ? msg.size() : sep;
  size_t pos = 0;
  while (pos < header_end) {
    const size_t end = msg.find("\r\n", pos);
    const size_t line_end = end == std::string::npos ? header_end : std::min(end, header_end);
    const std::string line = msg.substr(pos, line_end - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      for (char &ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (trim_copy(key) == "content-length") {
        return static_cast<size_t>(std::strtoul(trim_copy(line.substr(colon + 1)).c_str(), nullptr, 10));
      }
    }
    if (end == std::string::npos || end >= header_end) break;
    pos = end + 2;
  }
  return 0;
}

std::string sip_header_token(const std::string &raw) {
  std::string out;
  for (char ch : raw) {
    if (ch == '\r' || ch == '\n') continue;
    if (std::isalnum(static_cast<unsigned char>(ch)) ||
        ch == '_' || ch == '-' || ch == '.' || ch == ' ') {
      out.push_back(ch);
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
  return static_cast<uint32_t>(std::strtoul(number.c_str(), nullptr, 10));
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
  const size_t tag = value.find("tag=");
  if (tag == std::string::npos) return "";
  size_t begin = tag + 4;
  size_t end = begin;
  while (end < value.size() && value[end] != ';' && value[end] != ' ' && value[end] != '\r') end++;
  return value.substr(begin, end - begin);
}

std::string strip_angle_uri(const std::string &value) {
  std::string out = trim_copy(value);
  if (out.size() >= 2 && out.front() == '<' && out.back() == '>') {
    out = out.substr(1, out.size() - 2);
  }
  return out;
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
    if (semicolon != std::string::npos) uri = uri.substr(0, semicolon);
  }
  uri = trim_copy(uri);
  const char *prefix = "sip:";
  if (uri.rfind(prefix, 0) == 0) uri = uri.substr(4);
  const size_t at = uri.find('@');
  if (at == std::string::npos || at == 0) return "";
  return sip_uri_user_decode(uri.substr(0, at));
}

std::string response_via_with_rport(const std::string &via, uint32_t source_ip, uint16_t source_port) {
  std::string lowered = via;
  for (char &ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (lowered.find("rport") == std::string::npos) return via;
  struct in_addr a{};
  a.s_addr = htonl(source_ip);
  char ip_text[16];
  inet_ntoa_r(a, ip_text, sizeof(ip_text));
  const size_t first_semicolon = via.find(';');
  if (first_semicolon == std::string::npos) {
    return via + ";received=" + std::string(ip_text) + ";rport=" + std::to_string(source_port);
  }
  std::string out = via.substr(0, first_semicolon);
  size_t pos = first_semicolon + 1;
  while (pos <= via.size()) {
    const size_t end = via.find(';', pos);
    const size_t part_end = end == std::string::npos ? via.size() : end;
    const std::string part = trim_copy(via.substr(pos, part_end - pos));
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
  return out;
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
  const int pt = std::atoi(line.substr(colon + 1, space - colon - 1).c_str());
  if (pt < 0 || pt > 127) return false;
  const std::string spec = trim_copy(line.substr(space + 1));
  const size_t slash1 = spec.find('/');
  const size_t slash2 = slash1 == std::string::npos ? std::string::npos : spec.find('/', slash1 + 1);
  if (slash1 == std::string::npos) return false;
  const std::string enc = spec.substr(0, slash1);
  const uint32_t rate = static_cast<uint32_t>(std::strtoul(spec.substr(slash1 + 1, slash2 - slash1 - 1).c_str(), nullptr, 10));
  const uint8_t channels = slash2 == std::string::npos ? 1 : static_cast<uint8_t>(std::strtoul(spec.substr(slash2 + 1).c_str(), nullptr, 10));
  AudioFormat candidate;
  candidate.sample_rate = rate;
  candidate.channels = channels;
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

SipTransport::SipTransport(uint16_t sip_port, uint16_t rtp_port, size_t udp_max_payload, std::string remote_host,
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
  if (host.empty()) return false;
  struct in_addr a{};
  if (inet_aton(host.c_str(), &a) == 0) return false;
  this->remote_ip_v4_.store(ntohl(a.s_addr), std::memory_order_release);
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
  if (!this->bind_udp_(&this->sip_socket_, this->sip_port_, "SIP")) return false;
  if (!this->bind_tcp_(&this->sip_tcp_listener_socket_, this->sip_port_, "SIP")) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
    return false;
  }
  this->running_.store(true, std::memory_order_release);
  if (!audio_core::start_pinned_task(SipTransport::sip_task_trampoline_, "voip_sip",
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
  ESP_LOGI(TAG, "SIP listening on UDP+TCP/%u, RTP base UDP/%u", (unsigned) this->sip_port_, (unsigned) this->rtp_port_);
  this->emit_connection_change_(true);
  return true;
}

void SipTransport::request_tcp_client_close_() {
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
  const int socket = this->sip_tcp_client_socket_.exchange(-1, std::memory_order_acq_rel);
  this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
  if (socket >= 0) close(socket);
  this->sip_tcp_rx_buffer_.clear();
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
  if (this->rtp_task_handle_ != nullptr) {
    xTaskNotifyGive(this->rtp_task_handle_);
  }
  const int socket = this->rtp_socket_;
  if (socket < 0) return;
  struct sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  self.sin_port = htons(this->rtp_port_);
  sendto(socket, "", 0, 0, reinterpret_cast<struct sockaddr *>(&self), sizeof(self));
}

void SipTransport::stop() {
  this->stop_audio_path();
  if (!this->running_.exchange(false, std::memory_order_acq_rel)) return;
  if (this->sip_socket_ >= 0) {
    close(this->sip_socket_);
    this->sip_socket_ = -1;
  }
  if (this->sip_tcp_listener_socket_ >= 0) {
    close(this->sip_tcp_listener_socket_);
    this->sip_tcp_listener_socket_ = -1;
  }
  this->request_tcp_client_close_();
  audio_core::force_delete_pinned_task(&this->sip_task_handle_, &this->sip_task_stack_, kSipTaskStackBytes);
  this->close_tcp_client_from_sip_task_();
  this->emit_connection_change_(false);
}

bool SipTransport::is_connected() const {
  return this->running_.load(std::memory_order_acquire);
}

void SipTransport::disconnect() {
  this->stop_audio_path();
  this->reset_dialog_();
}

bool SipTransport::start_audio_path() {
  if (this->rtp_running_.load(std::memory_order_acquire)) return true;
  this->reset_rtp_latch_();
  if (!this->bind_udp_(&this->rtp_socket_, this->rtp_port_, "RTP")) return false;
  if (this->rtp_task_done_ == nullptr) {
    this->rtp_task_done_ = xSemaphoreCreateBinary();
  }
  if (this->rtp_task_done_ == nullptr) {
    close(this->rtp_socket_);
    this->rtp_socket_ = -1;
    return false;
  }
  xSemaphoreTake(this->rtp_task_done_, 0);
  this->rtp_running_.store(true, std::memory_order_release);
  if (!audio_core::start_pinned_task(SipTransport::rtp_task_trampoline_, "voip_rtp",
                                          kRtpTaskStackBytes, this, kRtpTaskPriority, 1,
                                          this->task_stacks_in_psram_, TAG,
                                          &this->rtp_task_handle_, &this->rtp_task_tcb_,
                                          &this->rtp_task_stack_)) {
    this->rtp_running_.store(false, std::memory_order_release);
    close(this->rtp_socket_);
    this->rtp_socket_ = -1;
    return false;
  }
  return true;
}

void SipTransport::stop_audio_path() {
  this->close_media_session_();
  if (!this->rtp_running_.exchange(false, std::memory_order_acq_rel)) return;
  this->wake_rtp_task_();
  if (this->rtp_task_done_ != nullptr && xSemaphoreTake(this->rtp_task_done_, pdMS_TO_TICKS(1000)) == pdTRUE) {
    audio_core::cleanup_pinned_task(&this->rtp_task_handle_, &this->rtp_task_stack_, kRtpTaskStackBytes);
  } else {
    ESP_LOGE(TAG, "RTP task did not stop cleanly; leaving task resources owned by FreeRTOS");
    this->rtp_task_handle_ = nullptr;
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
  this->clear_invite_transaction_();
  this->clear_bye_transaction_();
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
  this->call_id_.clear();
  this->local_tag_.clear();
  this->remote_tag_.clear();
  this->branch_.clear();
  this->local_uri_.clear();
  this->remote_uri_.clear();
  this->last_invite_via_.clear();
  this->last_invite_from_.clear();
  this->last_invite_to_.clear();
  this->last_invite_cseq_.clear();
  this->last_invite_cseq_number_ = 0;
  this->caller_route_.clear();
  this->caller_name_.clear();
  this->dest_route_.clear();
  this->dest_name_.clear();
  this->close_media_session_();
  this->outgoing_invite_pending_.store(false, std::memory_order_release);
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
  } else if (method == "BYE") {
    txn = &this->pending_bye_;
  }
  if (txn == nullptr) return;
  const uint32_t now = millis();
  txn->request = message;
  txn->ip_v4 = ip_v4;
  txn->port = port;
  txn->interval_ms = 500;
  txn->next_ms = now + txn->interval_ms;
  txn->retries = 0;
  this->wake_sip_task_();
}

void SipTransport::pump_udp_retransmits_() {
  if (this->remote_sip_tcp_.load(std::memory_order_acquire)) return;
  const uint32_t now = millis();
  auto pump = [this, now](UdpTransaction &txn, const char *method) {
    if (txn.empty() || txn.ip_v4 == 0 || txn.port == 0 || now - txn.next_ms >= 0x80000000UL) return;
    if (now < txn.next_ms) return;
    const uint8_t max_retries = std::strcmp(method, "BYE") == 0 ? 4 : 6;
    if (txn.retries >= max_retries) {
      ESP_LOGW(TAG, "SIP UDP %s retransmit limit reached", method);
      txn.request.clear();
      if (std::strcmp(method, "BYE") == 0) {
        this->reset_dialog_();
      }
      return;
    }
    if (this->send_sip_(txn.request, txn.ip_v4, txn.port)) {
      txn.retries++;
      ESP_LOGD(TAG, "SIP UDP %s retransmit #%u", method, (unsigned) txn.retries);
    }
    txn.interval_ms = std::min<uint16_t>(static_cast<uint16_t>(txn.interval_ms * 2), 4000);
    txn.next_ms = now + txn.interval_ms;
  };

  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    pump(this->pending_invite_, "INVITE");
  }
  pump(this->pending_bye_, "BYE");
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
  const int socket = this->sip_tcp_client_socket_.load(std::memory_order_acquire);
  if (message.empty()) return false;
  if (socket < 0) {
    if (this->remote_sip_tcp_.load(std::memory_order_acquire)) {
      LockGuard lock(this->tcp_tx_pending_mutex_);
      this->tcp_tx_pending_ = message;
      return true;
    }
    return false;
  }
  size_t sent_total = 0;
  while (sent_total < message.size()) {
    const int sent = send(socket, message.data() + sent_total, message.size() - sent_total, 0);
    if (sent <= 0) {
      const int err = errno;
      ESP_LOGW(TAG, "SIP TCP TX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
      return false;
    }
    sent_total += static_cast<size_t>(sent);
  }
  ESP_LOGI(TAG, "SIP TCP TX %u bytes", (unsigned) message.size());
  return true;
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
  return "v=0\r\n"
         "o=- 0 0 IN IP4 " + local_ip + "\r\n"
         "s=VoIP Stack\r\n"
         "c=IN IP4 " + local_ip + "\r\n"
         "t=0 0\r\n"
         "m=audio " + std::to_string(this->rtp_port_) + " RTP/AVP " + payloads + "\r\n" +
         maps +
         flows +
         "a=ptime:" + std::to_string(selected_ptime) + "\r\n"
         "a=maxptime:" + std::to_string(selected_ptime) + "\r\n"
         "a=sendrecv\r\n";
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
  return "v=0\r\n"
         "o=- 0 0 IN IP4 " + local_ip + "\r\n"
         "s=VoIP Stack\r\n"
         "c=IN IP4 " + local_ip + "\r\n"
         "t=0 0\r\n"
         "m=audio " + std::to_string(this->rtp_port_) + " RTP/AVP " + payloads + "\r\n" +
         maps +
         flows +
         "a=ptime:" + std::to_string(selected_rx.frame_ms) + "\r\n"
         "a=maxptime:" + std::to_string(selected_rx.frame_ms) + "\r\n"
         "a=sendrecv\r\n";
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
  size_t ptime_pos = 0;
  while (ptime_pos < sdp.size()) {
    size_t end = sdp.find("\r\n", ptime_pos);
    if (end == std::string::npos) end = sdp.size();
    const std::string line = sdp.substr(ptime_pos, end - ptime_pos);
    if (line.rfind("a=ptime:", 0) == 0) {
      const unsigned parsed = static_cast<unsigned>(std::strtoul(line.substr(8).c_str(), nullptr, 10));
      if (parsed == 10 || parsed == 16 || parsed == 20 || parsed == 32) media_ptime = static_cast<uint8_t>(parsed);
    } else if (line.rfind("a=x-voip-stack-flow:", 0) == 0) {
      const size_t value_start = sizeof("a=x-voip-stack-flow:") - 1;
      const size_t space = line.find(' ', value_start);
      if (space != std::string::npos) {
        const int parsed_pt = std::atoi(line.substr(value_start, space - value_start).c_str());
        const std::string flow = trim_copy(line.substr(space + 1));
        if (parsed_pt >= 0 && parsed_pt < 128) {
          uint8_t flags = 0;
          if (flow == "send" || flow == "sendrecv") flags |= 0x01;
          if (flow == "recv" || flow == "sendrecv") flags |= 0x02;
          payload_flow[parsed_pt] = flags;
        }
      }
    }
    if (end == sdp.size()) break;
    ptime_pos = end + 2;
  }
  size_t pos = 0;
  while (pos < sdp.size()) {
    size_t end = sdp.find("\r\n", pos);
    if (end == std::string::npos) end = sdp.size();
    const std::string line = sdp.substr(pos, end - pos);
    if (line.rfind("c=IN IP4 ", 0) == 0) {
      struct in_addr a{};
      if (inet_aton(line.substr(9).c_str(), &a) != 0 && a.s_addr != 0) media_ip = ntohl(a.s_addr);
    } else if (line.rfind("m=audio ", 0) == 0) {
      media_port = static_cast<uint16_t>(std::strtoul(line.substr(8).c_str(), nullptr, 10));
    } else if (line.rfind("a=rtpmap:", 0) == 0) {
      AudioFormat fmt;
      uint8_t pt = 0;
      if (parse_rtpmap_format(line, &fmt, &pt)) {
        fmt.frame_ms = media_ptime;
        AudioFormat local_rx;
        AudioFormat local_tx;
        const uint8_t flow = payload_flow[pt];
        const bool has_flow_for_payload = flow != 0;
        const bool peer_can_send = !has_flow_for_payload || (flow & 0x01) != 0;
        const bool peer_can_recv = !has_flow_for_payload || (flow & 0x02) != 0;
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
    ESP_LOGW(TAG, "SIP SDP rejected: body_len=%u media_port=%u media_ip=%08x selected_tx=%s selected_rx=%s",
             (unsigned) sdp.size(), (unsigned) media_port, (unsigned) media_ip,
             selected_tx ? "yes" : "no", selected_rx ? "yes" : "no");
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
  this->remote_ip_v4_.store(media_ip, std::memory_order_release);
  this->remote_rtp_port_.store(media_port, std::memory_order_release);
  return true;
}

bool SipTransport::send_request_(const std::string &method, const std::string &body, uint32_t cseq) {
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_sip_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0 || this->call_id_.empty()) return false;
  std::string branch;
  if (method == "INVITE") {
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
  const std::string request_uri = this->remote_uri_.empty()
      ? ("sip:voip@" + local_ip)
      : strip_angle_uri(this->remote_uri_);
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
  const uint32_t request_cseq = cseq == 0 ? this->cseq_++ : cseq;
  msg += "CSeq: " + std::to_string(request_cseq) + " " + method + "\r\n";
  msg += "Contact: " + this->local_uri_ + "\r\n";
  msg += "User-Agent: ESPHome-VoIP-Stack-SIP\r\n";
  if (method == "INVITE") {
    msg += "X-Voip-Stack-Caller-Route: " + this->caller_route_ + "\r\n";
    msg += "X-Voip-Stack-Caller-Name: " + this->caller_name_ + "\r\n";
    msg += "X-Voip-Stack-Dest-Route: " + this->dest_route_ + "\r\n";
    msg += "X-Voip-Stack-Dest-Name: " + this->dest_name_ + "\r\n";
  }
  if (!body.empty()) msg += "Content-Type: application/sdp\r\n";
  msg += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  msg += body;
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent) {
    if (method == "INVITE") this->mark_sip_event_(SipEvent::INVITE);
    else if (method == "ACK") this->mark_sip_event_(SipEvent::ACK);
    else if (method == "CANCEL") this->mark_sip_event_(SipEvent::CANCEL);
    else if (method == "BYE") this->mark_sip_event_(SipEvent::BYE);
    else if (method == "OPTIONS") this->mark_sip_event_(SipEvent::OPTIONS);
    if (method == "INVITE" || method == "BYE") {
      this->remember_udp_transaction_(method, msg, ip, port);
    }
  }
  return sent;
}

bool SipTransport::send_invite_error_ack_() {
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_sip_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0 || this->call_id_.empty() || this->branch_.empty()) return false;
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(ip, &local_ip);
  const std::string request_uri = this->remote_uri_.empty()
      ? ("sip:voip@" + local_ip)
      : strip_angle_uri(this->remote_uri_);
  const char *transport = this->remote_sip_tcp_.load(std::memory_order_acquire) ? "TCP" : "UDP";
  std::string msg = "ACK " + request_uri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/" + std::string(transport) + " " + local_ip + ":" +
         std::to_string(this->sip_port_) + ";branch=" + this->branch_ + ";rport\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: " + this->local_uri_ + ";tag=" + this->local_tag_ + "\r\n";
  msg += "To: " + this->remote_uri_;
  if (!this->remote_tag_.empty()) msg += ";tag=" + this->remote_tag_;
  msg += "\r\n";
  msg += "Call-ID: " + this->call_id_ + "\r\n";
  msg += "CSeq: " + std::to_string(this->invite_cseq_) + " ACK\r\n";
  msg += "Contact: " + this->local_uri_ + "\r\n";
  msg += "User-Agent: ESPHome-VoIP-Stack-SIP\r\n";
  msg += "Content-Length: 0\r\n\r\n";
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent) this->mark_sip_event_(SipEvent::ACK);
  return sent;
}

bool SipTransport::send_response_(uint16_t status, const char *reason, const std::string &body,
                                  const std::string &app_reason) {
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_sip_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0 || this->last_invite_via_.empty()) return false;
  std::string msg = "SIP/2.0 " + std::to_string(status) + " " + reason + "\r\n";
  msg += "Via: " + response_via_with_rport(this->last_invite_via_, ip, port) + "\r\n";
  msg += "From: " + this->last_invite_from_ + "\r\n";
  msg += "To: " + this->last_invite_to_;
  if (this->last_invite_to_.find("tag=") == std::string::npos) {
    if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
    msg += ";tag=" + this->local_tag_;
  }
  msg += "\r\n";
  msg += "Call-ID: " + this->call_id_ + "\r\n";
  msg += "CSeq: " + this->last_invite_cseq_ + "\r\n";
  msg += "Contact: " + this->local_uri_ + "\r\n";
  msg += "User-Agent: ESPHome-VoIP-Stack-SIP\r\n";
  const std::string clean_reason = sip_header_token(app_reason);
  if (!clean_reason.empty()) {
    msg += "Reason: X-Voip-Stack;cause=" + std::to_string(status) + ";text=" + sip_quoted(clean_reason) + "\r\n";
    msg += "X-Voip-Stack-Decline-Reason: " + clean_reason + "\r\n";
  }
  if (status == 405) {
    msg += "Allow: ACK, BYE, CANCEL, INVITE, OPTIONS\r\n";
  }
  if (!body.empty()) msg += "Content-Type: application/sdp\r\n";
  msg += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  msg += body;
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent) this->mark_sip_event_(SipEvent::RESPONSE, status);
  return sent;
}

bool SipTransport::send_stateless_response_(const std::string &request, const sockaddr_in &src,
                                            uint16_t status, const char *reason,
                                            const std::string &app_reason) {
  const uint32_t ip = ntohl(src.sin_addr.s_addr);
  const uint16_t port = ntohs(src.sin_port);
  const std::string via = header_value(request, "Via");
  const std::string from = header_value(request, "From");
  const std::string to = header_value(request, "To");
  const std::string call_id = header_value(request, "Call-ID");
  const std::string cseq = header_value(request, "CSeq");
  if (via.empty() || from.empty() || to.empty() || call_id.empty() || cseq.empty()) return false;
  std::string msg = "SIP/2.0 " + std::to_string(status) + " " + reason + "\r\n";
  msg += "Via: " + response_via_with_rport(via, ip, port) + "\r\n";
  msg += "From: " + from + "\r\n";
  msg += "To: " + to + "\r\n";
  msg += "Call-ID: " + call_id + "\r\n";
  msg += "CSeq: " + cseq + "\r\n";
  const std::string clean_reason = sip_header_token(app_reason);
  if (!clean_reason.empty() && status >= 300) {
    msg += "Reason: X-Voip-Stack;cause=" + std::to_string(status) + ";text=" + sip_quoted(clean_reason) + "\r\n";
    msg += "X-Voip-Stack-Decline-Reason: " + clean_reason + "\r\n";
  }
  if (status == 405) {
    msg += "Allow: ACK, BYE, CANCEL, INVITE, OPTIONS\r\n";
  }
  msg += "Content-Length: 0\r\n\r\n";
  const bool sent = this->send_sip_(msg, ip, port);
  if (sent) this->mark_sip_event_(SipEvent::RESPONSE, status);
  return sent;
}

bool SipTransport::send_invite(const std::string &call_id,
                               const std::string &caller_route,
                               const std::string &caller_name,
                               const std::string &dest_route,
                               const std::string &dest_name) {
  this->clear_udp_transactions_();
  this->reset_rtp_latch_();
  this->call_id_ = call_id;
  this->caller_route_ = caller_route;
  this->caller_name_ = caller_name;
  this->dest_route_ = dest_route;
  this->dest_name_ = dest_name;
  this->last_sip_status_code_.store(0, std::memory_order_release);
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  if (ip == 0) return false;
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
  ESP_LOGI(TAG, "SIP INVITE call_id=%s from=%s to=%s", this->call_id_.c_str(),
           this->caller_name_.c_str(), this->dest_name_.c_str());
  const std::string sdp = this->build_sdp_offer_();
  if (sdp.empty()) return false;
  const bool sent = this->send_request_("INVITE", sdp, this->invite_cseq_);
  if (sent) {
    this->outgoing_invite_pending_.store(true, std::memory_order_release);
    if (this->cseq_ <= this->invite_cseq_) this->cseq_ = this->invite_cseq_ + 1;
  }
  return sent;
}

void SipTransport::send_audio_frame(const uint8_t *pcm, size_t bytes) {
  if (!this->rtp_running_.load(std::memory_order_acquire) || this->rtp_socket_ < 0 || pcm == nullptr || bytes == 0) return;
  const uint32_t ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_rtp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return;
  AudioFormat tx_format;
  uint8_t tx_payload_type = 96;
  this->get_media_config_(&tx_format, nullptr, &tx_payload_type, nullptr);
  uint8_t packet[1500];
  packet[0] = 0x80;
  packet[1] = tx_payload_type & 0x7F;
  const uint16_t seq = this->rtp_sequence_.fetch_add(1, std::memory_order_acq_rel);
  packet[2] = static_cast<uint8_t>(seq >> 8);
  packet[3] = static_cast<uint8_t>(seq & 0xFF);
  const uint32_t ts = this->rtp_timestamp_.load(std::memory_order_acquire);
  packet[4] = static_cast<uint8_t>(ts >> 24);
  packet[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
  packet[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
  packet[7] = static_cast<uint8_t>(ts & 0xFF);
  packet[8] = static_cast<uint8_t>(this->rtp_ssrc_ >> 24);
  packet[9] = static_cast<uint8_t>((this->rtp_ssrc_ >> 16) & 0xFF);
  packet[10] = static_cast<uint8_t>((this->rtp_ssrc_ >> 8) & 0xFF);
  packet[11] = static_cast<uint8_t>(this->rtp_ssrc_ & 0xFF);
  const uint8_t bps = tx_format.container_bytes_per_sample();
  const size_t input_bytes = bytes;
  bytes = pcm_to_rtp_payload(pcm, bytes, tx_format, packet + 12, sizeof(packet) - 12);
  if (bytes == 0 || bytes > this->udp_max_payload_) return;
  const uint32_t samples = bps == 0 || tx_format.channels == 0
      ? 0
      : static_cast<uint32_t>(input_bytes / bps / tx_format.channels);
  this->rtp_timestamp_.store(ts + samples, std::memory_order_release);
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip);
  dest.sin_port = htons(port);
  const int sent = sendto(this->rtp_socket_, packet, 12 + bytes, 0,
                          reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  if (sent > 0) {
    this->rtp_tx_packets_.fetch_add(1, std::memory_order_acq_rel);
    this->rtp_tx_bytes_.fetch_add(static_cast<uint32_t>(sent), std::memory_order_acq_rel);
  }
}

bool SipTransport::send_ringing(const std::string &call_id) {
  if (!call_id.empty()) this->call_id_ = call_id;
  return this->send_response_(180, "Ringing");
}

bool SipTransport::send_answer(const std::string &call_id,
                               const AudioFormat &caller_to_dest_format,
                               const AudioFormat &dest_to_caller_format) {
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
  if (!call_id.empty()) this->call_id_ = call_id;
  if (!this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    return this->send_bye(call_id);
  }
  const bool sent = this->send_request_("CANCEL", "", this->invite_cseq_);
  this->reset_dialog_();
  return sent;
}

bool SipTransport::send_bye(const std::string &call_id) {
  if (!call_id.empty()) this->call_id_ = call_id;
  this->stop_audio_path();
  return this->send_request_("BYE");
}

bool SipTransport::send_final_response(const std::string &call_id,
                                       uint16_t status,
                                       const std::string &reason) {
  if (!call_id.empty()) this->call_id_ = call_id;
  if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) {
    const bool sent = this->send_request_("CANCEL", "", this->invite_cseq_);
    this->reset_dialog_();
    return sent;
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

bool SipTransport::replay_stateless_invite_final_(const std::string &request, const sockaddr_in &src,
                                                  const std::string &call_id) {
  if (call_id.empty() || call_id != this->last_stateless_invite_final_call_id_) {
    return false;
  }
  const uint32_t age_ms = millis() - this->last_stateless_invite_final_ms_;
  if (this->last_stateless_invite_final_ms_ == 0 || age_ms > 5000) {
    return false;
  }
  ESP_LOGI(TAG, "SIP INVITE retransmission replaying %u %s for call_id=%s",
           this->last_stateless_invite_final_status_,
           this->last_stateless_invite_final_reason_.c_str(),
           call_id.c_str());
  return this->send_stateless_response_(
      request, src,
      this->last_stateless_invite_final_status_,
      this->last_stateless_invite_final_reason_.empty()
          ? "Busy Here"
          : this->last_stateless_invite_final_reason_.c_str(),
      this->last_stateless_invite_final_app_reason_);
}

void SipTransport::remember_stateless_invite_final_(const std::string &call_id, uint16_t status,
                                                    const char *reason, const std::string &app_reason) {
  this->last_stateless_invite_final_call_id_ = call_id;
  this->last_stateless_invite_final_status_ = status;
  this->last_stateless_invite_final_reason_ = reason == nullptr ? "" : reason;
  this->last_stateless_invite_final_app_reason_ = app_reason;
  this->last_stateless_invite_final_ms_ = millis();
}

bool SipTransport::handle_invite_(const std::string &message, const sockaddr_in &src) {
  const std::string body = message_body(message);
  const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
  const std::string incoming_call_id = header_value(message, "Call-ID");
  if (this->replay_stateless_invite_final_(message, src, incoming_call_id)) {
    return true;
  }
  if (!incoming_call_id.empty() && !this->call_id_.empty() && incoming_call_id != this->call_id_) {
    ESP_LOGW(TAG, "SIP INVITE rejected busy: active_call_id=%s incoming_call_id=%s",
             this->call_id_.c_str(), incoming_call_id.c_str());
    this->remember_stateless_invite_final_(incoming_call_id, 486, "Busy Here", "busy");
    return this->send_stateless_response_(message, src, 486, "Busy Here", "busy");
  }
  const std::string incoming_cseq = header_value(message, "CSeq");
  const uint32_t incoming_cseq_number = cseq_number(incoming_cseq);
  if (!incoming_call_id.empty() && incoming_call_id == this->call_id_ &&
      this->last_invite_cseq_number_ != 0 &&
      incoming_cseq_number != this->last_invite_cseq_number_) {
    ESP_LOGW(TAG, "SIP re-INVITE rejected: call_id=%s old_cseq=%u new_cseq=%u",
             incoming_call_id.c_str(),
             (unsigned) this->last_invite_cseq_number_,
             (unsigned) incoming_cseq_number);
    return this->send_stateless_response_(message, src, 488, "Not Acceptable Here", "reinvite_unsupported");
  }
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  this->call_id_ = incoming_call_id;
  this->last_invite_via_ = header_value(message, "Via");
  this->last_invite_from_ = header_value(message, "From");
  this->last_invite_to_ = header_value(message, "To");
  this->last_invite_cseq_ = incoming_cseq;
  this->last_invite_cseq_number_ = incoming_cseq_number;
  this->remote_tag_ = tag_from_header(this->last_invite_from_);
  if (this->local_tag_.empty()) this->local_tag_ = make_token("tag");
  this->remote_uri_ = this->last_invite_from_;
  this->local_uri_ = this->last_invite_to_;
  if (this->call_id_.empty() || this->last_invite_via_.empty() || this->last_invite_from_.empty() ||
      this->last_invite_to_.empty() || this->last_invite_cseq_.empty()) {
    const bool sent = this->send_response_(400, "Bad Request");
    this->reset_dialog_();
    return sent;
  }
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

  std::string from_user = sip_header_token(header_value(message, "X-Voip-Stack-Caller-Name"));
  std::string to_user = sip_header_token(header_value(message, "X-Voip-Stack-Dest-Name"));
  if (from_user.empty()) from_user = sip_user_from_header(this->last_invite_from_);
  if (to_user.empty()) to_user = sip_user_from_header(this->last_invite_to_);
  if (from_user.empty() || to_user.empty()) {
    const bool sent = this->send_response_(400, "Bad Request");
    this->reset_dialog_();
    return sent;
  }
  this->caller_name_ = from_user;
  this->dest_name_ = to_user;
  this->caller_route_ = header_value(message, "X-Voip-Stack-Caller-Route");
  this->dest_route_ = header_value(message, "X-Voip-Stack-Dest-Route");
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
  this->remote_ip_v4_.store(src_ip, std::memory_order_release);
  this->remote_sip_port_.store(ntohs(src.sin_port), std::memory_order_release);
  if (message.rfind("SIP/2.0 ", 0) != 0 || message.size() < 12) return false;
  const int status = std::atoi(message.substr(8, 3).c_str());
  this->mark_sip_event_(SipEvent::RESPONSE, static_cast<uint16_t>(status));
  const std::string response_call_id = header_value(message, "Call-ID");
  if (response_call_id.empty() || this->call_id_.empty() || response_call_id != this->call_id_) {
    ESP_LOGD(TAG, "SIP response ignored for stale/unknown call_id=%s current=%s",
             response_call_id.empty() ? "(empty)" : response_call_id.c_str(),
             this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
    return true;
  }
  const std::string method = cseq_method(header_value(message, "CSeq"));
  if (method == "INVITE" && status >= 100) {
    this->clear_invite_transaction_();
  }
  if (status == 180 && method == "INVITE") {
    SipSignal signal;
    signal.type = SipSignalType::STATUS_180_RINGING;
    signal.status_code = 180;
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
      return true;
    }
    if (method != "INVITE") {
      ESP_LOGI(TAG, "SIP %u response for %s ignored", status, method.c_str());
      return true;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    const std::string to = header_value(message, "To");
    this->remote_tag_ = tag_from_header(to);
    this->learn_remote_rtp_from_sdp_(message_body(message), src_ip);
    this->send_request_("ACK", "", this->invite_cseq_);
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
      return true;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    this->remote_tag_ = tag_from_header(header_value(message, "To"));
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
  const std::string msg(data, len);
  if (msg.rfind("SIP/2.0 ", 0) == 0) {
    this->handle_response_(msg, src);
    return;
  }
  const size_t first_space = msg.find(' ');
  const std::string method = first_space == std::string::npos ? "" : msg.substr(0, first_space);
  ESP_LOGI(TAG, "SIP RX method=%s len=%u", method.c_str(), (unsigned) len);
  if (method == "INVITE") {
    this->mark_sip_event_(SipEvent::INVITE);
    this->handle_invite_(msg, src);
  } else if (method == "ACK") {
    this->mark_sip_event_(SipEvent::ACK);
    const std::string request_call_id = header_value(msg, "Call-ID");
    if (request_call_id.empty() || this->call_id_.empty() || request_call_id != this->call_id_) {
      ESP_LOGD(TAG, "SIP ACK ignored for stale/unknown call_id=%s current=%s",
               request_call_id.empty() ? "(empty)" : request_call_id.c_str(),
               this->call_id_.empty() ? "(none)" : this->call_id_.c_str());
      return;
    }
    this->outgoing_invite_pending_.store(false, std::memory_order_release);
    this->open_media_session_();
  } else if (method == "BYE") {
    this->mark_sip_event_(SipEvent::BYE);
    const std::string request_call_id = header_value(msg, "Call-ID");
    if (!request_call_id.empty() && !this->call_id_.empty() && request_call_id != this->call_id_) {
      ESP_LOGW(TAG, "SIP BYE ignored for stale call_id=%s current=%s",
               request_call_id.c_str(), this->call_id_.c_str());
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    this->send_stateless_response_(msg, src, 200, "OK");
    SipSignal signal;
    signal.type = SipSignalType::BYE;
    signal.call_id = this->call_id_;
    this->emit_sip_signal_(signal);
    this->reset_dialog_();
  } else if (method == "CANCEL") {
    this->mark_sip_event_(SipEvent::CANCEL);
    const std::string request_call_id = header_value(msg, "Call-ID");
    if (!request_call_id.empty() && !this->call_id_.empty() && request_call_id != this->call_id_) {
      ESP_LOGW(TAG, "SIP CANCEL ignored for stale call_id=%s current=%s",
               request_call_id.c_str(), this->call_id_.c_str());
      this->send_stateless_response_(msg, src, 481, "Call/Transaction Does Not Exist");
      return;
    }
    this->send_stateless_response_(msg, src, 200, "OK");
    this->send_response_(487, "Request Terminated");
    SipSignal signal;
    signal.type = SipSignalType::CANCEL;
    signal.status_code = 487;
    signal.call_id = this->call_id_;
    signal.reason = "cancelled";
    this->emit_sip_signal_(signal);
    this->reset_dialog_();
  } else if (method == "OPTIONS") {
    this->mark_sip_event_(SipEvent::OPTIONS);
    this->send_stateless_response_(msg, src, 200, "OK");
  } else if (sip_method_known_(method)) {
    this->send_stateless_response_(msg, src, 405, "Method Not Allowed");
  } else {
    this->send_stateless_response_(msg, src, 501, "Not Implemented");
  }
}

void SipTransport::handle_sip_stream_(int socket, const sockaddr_in &src) {
  char buf[1024];
  while (true) {
    const int n = recv(socket, buf, sizeof(buf), 0);
    if (n > 0) {
      this->sip_tcp_rx_buffer_.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {
      ESP_LOGI(TAG, "SIP TCP peer closed");
      close(socket);
      int expected = socket;
      this->sip_tcp_client_socket_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
      this->remote_sip_tcp_.store(false, std::memory_order_release);
      this->sip_tcp_rx_buffer_.clear();
      return;
    }
    const int err = errno;
    if (err == EWOULDBLOCK || err == EAGAIN || err == ENOTCONN || err == EINPROGRESS || err == EALREADY) break;
    ESP_LOGW(TAG, "SIP TCP RX failed: %s (%d: %s)", socket_errno_name(err), err, socket_errno_text(err));
    close(socket);
    int expected = socket;
    this->sip_tcp_client_socket_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
    this->remote_sip_tcp_.store(false, std::memory_order_release);
    this->sip_tcp_rx_buffer_.clear();
    return;
  }

  while (true) {
    const size_t sep = this->sip_tcp_rx_buffer_.find("\r\n\r\n");
    if (sep == std::string::npos) return;
    const size_t body_len = sip_content_length(this->sip_tcp_rx_buffer_);
    const size_t total = sep + 4 + body_len;
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
    this->emit_connection_change_(false);
  };
  auto promote_tcp_connect = [&]() {
    this->sip_tcp_client_socket_.store(connecting_fd, std::memory_order_release);
    this->sip_tcp_client_close_requested_.store(false, std::memory_order_release);
    this->sip_tcp_rx_buffer_.clear();
    char ip[16];
    struct in_addr a{};
    a.s_addr = htonl(connecting_ip_v4);
    inet_ntoa_r(a, ip, sizeof(ip));
    ESP_LOGI(TAG, "SIP TCP originate connected to %s:%u", ip, (unsigned) connecting_port);
    connecting_fd = -1;
    connect_deadline_ms = 0;
    connecting_ip_v4 = 0;
    connecting_port = 0;
    this->tcp_connect_requested_.store(false, std::memory_order_release);
    std::string pending;
    {
      LockGuard lock(this->tcp_tx_pending_mutex_);
      pending.swap(this->tcp_tx_pending_);
    }
    if (!pending.empty()) {
      this->send_sip_tcp_(pending);
    }
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
    if (connecting_fd >= 0) {
      const uint32_t now = millis();
      timeout_ms = now - connect_deadline_ms < 0x80000000UL && now < connect_deadline_ms
                       ? connect_deadline_ms - now
                       : 0;
      timeout_ptr = &timeout;
    }
    const bool udp_timer_pending =
        !this->remote_sip_tcp_.load(std::memory_order_acquire) &&
        ((this->outgoing_invite_pending_.load(std::memory_order_acquire) && !this->pending_invite_.empty()) ||
         !this->pending_bye_.empty());
    if (udp_timer_pending) {
      const uint32_t now = millis();
      uint32_t next_ms = 0;
      auto include_txn = [now, &next_ms](const UdpTransaction &txn) {
        if (txn.empty()) return;
        const uint32_t delta = txn.next_ms - now;
        if (now - txn.next_ms < 0x80000000UL && now >= txn.next_ms) {
          next_ms = 0;
        } else if (next_ms == 0 || delta < next_ms) {
          next_ms = delta;
        }
      };
      if (this->outgoing_invite_pending_.load(std::memory_order_acquire)) include_txn(this->pending_invite_);
      include_txn(this->pending_bye_);
      if (timeout_ptr == nullptr || next_ms < timeout_ms) {
        timeout_ms = next_ms;
        timeout_ptr = &timeout;
      }
    }
    if (timeout_ptr != nullptr) {
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_usec = (timeout_ms % 1000) * 1000;
    }
    const int ready = select(max_fd + 1, &readfds, &writefds, nullptr, timeout_ptr);
    if (connecting_fd >= 0 && millis() - connect_deadline_ms < 0x80000000UL && millis() >= connect_deadline_ms) {
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
        this->close_tcp_client_from_sip_task_();
        this->sip_tcp_client_socket_.store(client, std::memory_order_release);
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
  vTaskDelete(nullptr);
}

void SipTransport::rtp_task_() {
  uint8_t buf[1600];
  uint8_t pcm[1500];
  while (this->rtp_running_.load(std::memory_order_acquire)) {
    const int socket = this->rtp_socket_;
    if (socket < 0) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(socket, &readfds);
    const int ready = select(socket + 1, &readfds, nullptr, nullptr, nullptr);
    if (ready <= 0 || !FD_ISSET(socket, &readfds)) {
      continue;
    }
    struct sockaddr_in src{};
    socklen_t slen = sizeof(src);
    int n = recvfrom(socket, buf, sizeof(buf), 0,
                     reinterpret_cast<struct sockaddr *>(&src), &slen);
    if (n > 12 && (buf[0] & 0xC0) == 0x80) {
      if (!this->media_active_.load(std::memory_order_acquire)) {
        continue;
      }
      const uint32_t src_ip = ntohl(src.sin_addr.s_addr);
      const uint16_t src_port = ntohs(src.sin_port);
      const uint32_t expected_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
      if (expected_ip != 0 && src_ip != expected_ip) {
        continue;
      }
      const uint32_t ssrc = (static_cast<uint32_t>(buf[8]) << 24) |
                            (static_cast<uint32_t>(buf[9]) << 16) |
                            (static_cast<uint32_t>(buf[10]) << 8) |
                            static_cast<uint32_t>(buf[11]);
      if (!this->rtp_ssrc_latched_.load(std::memory_order_acquire)) {
        this->latched_rtp_ip_v4_.store(src_ip, std::memory_order_release);
        this->latched_rtp_port_.store(src_port, std::memory_order_release);
        this->latched_rtp_ssrc_.store(ssrc, std::memory_order_release);
        this->rtp_ssrc_latched_.store(true, std::memory_order_release);
      } else {
        if (this->latched_rtp_ip_v4_.load(std::memory_order_acquire) != src_ip ||
            this->latched_rtp_port_.load(std::memory_order_acquire) != src_port ||
            this->latched_rtp_ssrc_.load(std::memory_order_acquire) != ssrc) {
          continue;
        }
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
      const size_t out_len = rtp_payload_to_pcm(payload, payload_len, rx_format, pcm, sizeof(pcm));
      if (out_len == 0) continue;
      this->rtp_rx_packets_.fetch_add(1, std::memory_order_acq_rel);
      this->rtp_rx_bytes_.fetch_add(static_cast<uint32_t>(n), std::memory_order_acq_rel);
      this->emit_audio_frame_(pcm, out_len, sequence, timestamp);
    }
  }
  const int socket = this->rtp_socket_;
  if (socket >= 0) {
    close(socket);
    this->rtp_socket_ = -1;
  }
  if (this->rtp_task_done_ != nullptr) {
    xSemaphoreGive(this->rtp_task_done_);
  }
  vTaskDelete(nullptr);
}

}  // namespace voip_stack
}  // namespace esphome

#endif  // USE_ESP32 && USE_ESPHOME_VOIP_SIP_TRANSPORT
