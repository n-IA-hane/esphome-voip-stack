#include "sip_transport.h"
#include "sip_message.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include <algorithm>
#include <cstring>

#include <lwip/sockets.h>

#include "esphome/core/log.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.sip";

std::string SipTransport::wrap_sdp_envelope_(const std::string &local_ip, const std::string &payloads,
                                             const std::string &maps, const std::string &flows,
                                             uint8_t ptime) const {
  return "v=0\r\n"
         "o=- " + std::to_string(this->sdp_session_id_) + " " +
         std::to_string(this->sdp_session_version_) + " IN IP4 " +
         local_ip + "\r\n"
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

void SipTransport::capture_remote_media_shape_(const std::string &sdp) {
  this->remote_media_shape_.clear();
  size_t pos = 0;
  while (pos < sdp.size()) {
    size_t end = sdp.find("\r\n", pos);
    if (end == std::string::npos) end = sdp.size();
    const std::string line = sdp.substr(pos, end - pos);
    if (line.rfind("m=", 0) == 0) {
      const int8_t index = this->remote_media_shape_.count;
      if (this->remote_media_shape_.append(line)) {
        if (this->remote_media_shape_.audio_index < 0) {
          bool payloads[128]{};
          uint16_t port = 0;
          if (parse_audio_media_line(line, &port, payloads))
            this->remote_media_shape_.audio_index = index;
        }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
        if (this->remote_media_shape_.video_index < 0) {
          bool payloads[128]{};
          uint16_t port = 0;
          if (parse_video_media_line(line, &port, payloads))
            this->remote_media_shape_.video_index = index;
        }
#endif
      }
    }
    if (end == sdp.size()) break;
    pos = end + 2;
  }
}

std::string SipTransport::rejected_media_answer_(
    const std::string &media_line) const {
  if (media_line.rfind("m=", 0) != 0) return "";
  const size_t port_start = media_line.find(' ');
  if (port_start == std::string::npos) return "";
  const size_t port_end = media_line.find_first_of(" \t", port_start + 1);
  if (port_end == std::string::npos) return "";
  return media_line.substr(0, port_start + 1) + "0" +
         media_line.substr(port_end) + "\r\na=inactive\r\n";
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
std::string SipTransport::append_video_sdp_(const std::string &sdp,
                                            const std::string &local_ip,
                                            bool answer) const {
  if (this->video_source_ == nullptr && this->video_sink_ == nullptr) {
    return sdp;
  }
  if (answer && this->video_offered_ && !this->video_negotiated_) {
    return sdp + "m=video 0 RTP/AVP " +
           std::to_string(this->negotiated_video_capability_.payload_type) +
           "\r\n";
  }
  const VideoCapability local_send = this->local_video_send_capability_();
  const VideoCapability local_receive =
      this->local_video_receive_capability_();
  const bool offer_send =
      this->video_send_requested_.load(std::memory_order_acquire) &&
      this->video_source_ != nullptr && local_send.valid();
  const bool offer_receive =
      this->video_sink_ != nullptr && local_receive.valid();
  // RFC 6184 profile-level-id in a unicast offer declares the highest level
  // the offerer can receive. With bilateral level asymmetry the answer
  // independently declares the answerer's receive level, allowing a P4
  // hardware encoder and software decoder to use different envelopes.
  VideoCapability capability =
      answer ? this->negotiated_video_capability_
             : offer_receive ? local_receive : local_send;
  if (!capability.valid()) {
    ESP_LOGW(TAG, "Video SDP omitted: local capabilities are invalid");
    return sdp;
  }
  const bool send =
      answer ? this->video_send_enabled_
             : offer_send &&
                   local_send.encoding == capability.encoding;
  const bool receive =
      answer ? this->video_receive_enabled_
             : offer_receive &&
                   local_receive.encoding == capability.encoding;
  if (!send && !receive && !answer) return sdp;
  const char *direction =
      send && receive ? "sendrecv"
                      : send ? "sendonly" : receive ? "recvonly" : "inactive";
  std::string out = sdp;
  out += "m=video " + std::to_string(this->video_rtp_port_) +
         " RTP/AVP " + std::to_string(capability.payload_type) + "\r\n";
  out += "c=IN IP4 " + local_ip + "\r\n";
  if (capability.max_bitrate_bps != 0) {
    out += "b=TIAS:" + std::to_string(capability.max_bitrate_bps) + "\r\n";
  }
  out += "a=rtcp:" +
         std::to_string(static_cast<uint16_t>(this->video_rtp_port_ + 1)) +
         " IN IP4 " + local_ip + "\r\n";
  out += "a=rtpmap:" + std::to_string(capability.payload_type) +
         " " + capability.encoding + "/90000\r\n";
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  if (capability.is_h264()) {
    out += "a=fmtp:" + std::to_string(capability.payload_type) +
           " packetization-mode=1;profile-level-id=" +
           capability.profile_level_id;
    if (capability.level_asymmetry_allowed)
      out += ";level-asymmetry-allowed=1";
    out += "\r\n";
    if (capability.rtcp_feedback_pli) {
      out += "a=rtcp-fb:" + std::to_string(capability.payload_type) +
             " nack pli\r\n";
    }
    if (capability.rtcp_feedback_fir) {
      out += "a=rtcp-fb:" + std::to_string(capability.payload_type) +
             " ccm fir\r\n";
    }
  }
#endif
  // RFC 8866 media-level recommendation. A software video sink can advertise
  // a lower receive budget than the hardware encoder without a private fmtp.
  out += "a=framerate:" + std::to_string(capability.max_fps) + "\r\n";
  out += std::string("a=") + direction + "\r\n";
  return out;
}
#endif

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
  std::string sdp =
      this->wrap_sdp_envelope_(local_ip, payloads, maps, flows, selected_ptime);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  sdp = this->append_video_sdp_(sdp, local_ip, false);
#endif
  return sdp;
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
  const std::string audio_sdp = this->wrap_sdp_envelope_(
      local_ip, payloads, maps, flows, selected_rx.frame_ms);
  const size_t audio_media = audio_sdp.find("m=audio ");
  if (audio_media == std::string::npos) return "";
  const std::string session = audio_sdp.substr(0, audio_media);
  const std::string audio = audio_sdp.substr(audio_media);
  std::string video;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  video = this->append_video_sdp_("", local_ip, true);
#endif
  if (this->remote_media_shape_.empty()) return audio_sdp + video;
  if (this->remote_media_shape_.overflow) {
    ESP_LOGW(TAG, "SIP SDP answer refused: more than %u media lines",
             (unsigned) kMaxSdpMediaLines);
    return "";
  }

  // RFC 3264 section 6 requires exactly one answer m-line for every offered
  // m-line, in the same order. Unsupported and duplicate streams are retained
  // with port zero instead of being silently omitted or reordered.
  std::string answer = session;
  for (size_t index = 0; index < this->remote_media_shape_.count; index++) {
    if (static_cast<int>(index) == this->remote_media_shape_.audio_index) {
      answer += audio;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    } else if (static_cast<int>(index) == this->remote_media_shape_.video_index &&
               !video.empty()) {
      answer += video;
#endif
    } else {
      const std::string rejected = this->rejected_media_answer_(
          this->remote_media_shape_.line(index));
      if (rejected.empty()) return "";
      answer += rejected;
    }
  }
  return answer;
}

bool SipTransport::learn_remote_rtp_from_sdp_(const std::string &sdp,
                                              uint32_t default_ip,
                                              bool remote_is_answer) {
  this->capture_remote_media_shape_(sdp);
  if (this->remote_media_shape_.overflow) {
    ESP_LOGW(TAG, "SIP SDP rejected: too many media lines");
    return false;
  }
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
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (!this->learn_remote_video_from_sdp_(sdp, default_ip,
                                          remote_is_answer)) {
    // An unsupported video m-line is rejected independently. Audio remains
    // valid and build_sdp_answer_ will return m=video 0.
    ESP_LOGW(TAG, "SIP SDP video rejected while retaining negotiated audio");
  }
#endif
  return true;
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
bool SipTransport::learn_remote_video_from_sdp_(const std::string &sdp,
                                                uint32_t default_ip,
                                                bool remote_is_answer) {
  this->reset_video_negotiation_();
  bool offered_payloads[128]{};
  uint8_t offered_payload_order[128]{};
  size_t offered_payload_count = 0;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  bool h264_payloads[128]{};
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  bool jpeg_payloads[128]{};
#endif
  bool rtpmap_seen[128]{};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
  char profiles[128][7]{};
  bool profile_valid[128]{};
  uint8_t packetization_modes[128]{};
  bool level_asymmetry_allowed[128]{};
  bool rtcp_feedback_pli[128]{};
  bool rtcp_feedback_fir[128]{};
#endif
  uint8_t media_max_fps = 0;
  uint32_t media_max_bitrate_bps = 0;
  uint16_t media_port = 0;
  uint32_t media_ip = default_ip;
  uint32_t session_ip = default_ip;
  uint16_t media_rtcp_port = 0;
  uint32_t media_rtcp_ip = default_ip;
  bool media_rtcp_address_explicit = false;
  bool media_rtcp_attribute_invalid = false;
  bool rtcp_mux_only = false;
  uint8_t media_direction = 0x03;  // peer send + receive
  uint8_t session_direction = 0x03;
  bool seen_media = false;
  bool in_video = false;
  bool selected_video_line = false;

  size_t pos = 0;
  while (pos < sdp.size()) {
    size_t end = sdp.find("\r\n", pos);
    if (end == std::string::npos) end = sdp.size();
    const std::string line = sdp.substr(pos, end - pos);
    if (line.rfind("m=", 0) == 0) {
      seen_media = true;
      in_video = false;
      if (!selected_video_line) {
        bool candidate_payloads[128]{};
        uint16_t candidate_port = 0;
        uint8_t candidate_order[128]{};
        size_t candidate_count = 0;
        if (parse_video_media_line(line, &candidate_port,
                                   candidate_payloads, candidate_order,
                                   &candidate_count)) {
          selected_video_line = true;
          this->video_offered_ = true;
          media_port = candidate_port;
          std::copy(candidate_payloads, candidate_payloads + 128,
                    offered_payloads);
          std::copy(candidate_order, candidate_order + candidate_count,
                    offered_payload_order);
          offered_payload_count = candidate_count;
          // Keep the first offered PT even when the stream is disabled or
          // rejected before codec matching. RFC 3264 still requires the
          // corresponding answer m-line, with port zero and a payload format
          // taken from the offer rather than our unrelated local default.
          if (candidate_count > 0) {
            this->negotiated_video_capability_.payload_type =
                candidate_order[0];
          }
          media_ip = session_ip;
          media_rtcp_ip = session_ip;
          media_rtcp_port =
              candidate_port == UINT16_MAX
                  ? 0
                  : static_cast<uint16_t>(candidate_port + 1);
          media_rtcp_address_explicit = false;
          media_direction = session_direction;
          in_video = true;
        }
      }
    } else if (!seen_media && line.rfind("c=IN IP4 ", 0) == 0) {
      struct in_addr address{};
      if (inet_aton(line.substr(9).c_str(), &address) != 0 &&
          address.s_addr != 0) {
        session_ip = ntohl(address.s_addr);
        media_ip = session_ip;
        media_rtcp_ip = session_ip;
      }
    } else if (line.rfind("a=sendonly", 0) == 0 &&
               (!seen_media || in_video)) {
      if (!seen_media) session_direction = 0x01;
      if (in_video) media_direction = 0x01;
    } else if (line.rfind("a=recvonly", 0) == 0 &&
               (!seen_media || in_video)) {
      if (!seen_media) session_direction = 0x02;
      if (in_video) media_direction = 0x02;
    } else if (line.rfind("a=inactive", 0) == 0 &&
               (!seen_media || in_video)) {
      if (!seen_media) session_direction = 0;
      if (in_video) media_direction = 0;
    } else if (line.rfind("a=sendrecv", 0) == 0 &&
               (!seen_media || in_video)) {
      if (!seen_media) session_direction = 0x03;
      if (in_video) media_direction = 0x03;
    } else if (in_video && line.rfind("c=IN IP4 ", 0) == 0) {
      struct in_addr address{};
      if (inet_aton(line.substr(9).c_str(), &address) != 0 &&
          address.s_addr != 0) {
        media_ip = ntohl(address.s_addr);
        if (!media_rtcp_address_explicit) media_rtcp_ip = media_ip;
      }
    } else if (in_video && line.rfind("a=rtcp:", 0) == 0) {
      uint16_t parsed_port = 0;
      uint32_t parsed_ip = media_ip;
      bool has_address = false;
      if (parse_rtcp_attribute(line, &parsed_port, &parsed_ip,
                               &has_address)) {
        media_rtcp_port = parsed_port;
        media_rtcp_ip = has_address ? parsed_ip : media_ip;
        media_rtcp_address_explicit = has_address;
      } else {
        media_rtcp_attribute_invalid = true;
      }
    } else if (in_video && line == "a=rtcp-mux-only") {
      rtcp_mux_only = true;
    } else if (in_video && line.rfind("a=rtpmap:", 0) == 0) {
      uint8_t payload_type = 0;
      std::string encoding;
      if (parse_video_rtpmap(line, &payload_type, &encoding) &&
          offered_payloads[payload_type]) {
        rtpmap_seen[payload_type] = true;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
        h264_payloads[payload_type] = encoding == "H264/90000";
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
        jpeg_payloads[payload_type] = encoding == "JPEG/90000";
#endif
      }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    } else if (in_video && line.rfind("a=fmtp:", 0) == 0) {
      uint8_t payload_type = 0;
      const std::string profile =
          fmtp_parameter(line, "profile-level-id", &payload_type);
      if (offered_payloads[payload_type] && profile.size() == 6) {
        memcpy(profiles[payload_type], profile.data(), 6);
        profile_valid[payload_type] = true;
      }
      uint8_t mode_payload = 0;
      const std::string mode =
          fmtp_parameter(line, "packetization-mode", &mode_payload);
      if (offered_payloads[mode_payload] && mode == "1")
        packetization_modes[mode_payload] = 1;
      uint8_t asymmetry_payload = 0;
      const std::string asymmetry =
          fmtp_parameter(line, "level-asymmetry-allowed",
                         &asymmetry_payload);
      if (offered_payloads[asymmetry_payload] && asymmetry == "1")
        level_asymmetry_allowed[asymmetry_payload] = true;
    } else if (in_video && line.rfind("a=rtcp-fb:", 0) == 0) {
      parse_video_rtcp_feedback(line, offered_payloads,
                                rtcp_feedback_pli,
                                rtcp_feedback_fir);
#endif
    } else if (in_video && line.rfind("b=TIAS:", 0) == 0) {
      uint32_t parsed_bitrate = 0;
      if (parse_decimal_u32(trim_copy(line.substr(7)), UINT32_MAX,
                            &parsed_bitrate) &&
          parsed_bitrate > 0) {
        media_max_bitrate_bps = parsed_bitrate;
      }
    } else if (in_video && line.rfind("a=framerate:", 0) == 0) {
      uint32_t parsed_fps = 0;
      if (parse_decimal_u32(trim_copy(line.substr(12)), UINT8_MAX,
                            &parsed_fps) &&
          parsed_fps > 0) {
        media_max_fps = static_cast<uint8_t>(parsed_fps);
      }
    }
    if (end == sdp.size()) break;
    pos = end + 2;
  }

  if (!this->video_offered_) return true;
  // A disabled answer is not an error and must not disturb audio.
  if (media_port == 0) return true;
  // This implementation owns a distinct RTCP socket. Do not claim support for
  // a peer that requires multiplexing both protocols on one port.
  if (rtcp_mux_only || media_rtcp_attribute_invalid ||
      media_rtcp_port == 0 || media_rtcp_ip == 0) {
    return false;
  }
  // RFC 3551 statically assigns PT 26 to JPEG/90000, so rtpmap is optional.
  // An explicit conflicting rtpmap always wins and makes PT 26 incompatible.
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (offered_payloads[26] && !rtpmap_seen[26])
    jpeg_payloads[26] = true;
#endif

  const VideoCapability local_send = this->local_video_send_capability_();
  const VideoCapability local_receive =
      this->local_video_receive_capability_();
  for (size_t payload_index = 0; payload_index < offered_payload_count;
       payload_index++) {
    const uint8_t pt = offered_payload_order[payload_index];
    if (remote_is_answer && pt != this->video_offer_payload_type_) continue;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    const bool jpeg = jpeg_payloads[pt] && pt == 26;
    const bool h264 = false;
#else
    const bool jpeg = false;
    const bool h264 = h264_payloads[pt] &&
                      packetization_modes[pt] == 1 && profile_valid[pt];
#endif
    if (!jpeg && !h264) continue;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    const std::string peer_profile(profiles[pt], 6);
#else
    const std::string peer_profile;
#endif
    const bool peer_sends = (media_direction & 0x01) != 0;
    const bool peer_receives = (media_direction & 0x02) != 0;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    const bool bilateral_asymmetry =
        level_asymmetry_allowed[pt] &&
        (!peer_sends || local_receive.level_asymmetry_allowed) &&
        (!peer_receives || local_send.level_asymmetry_allowed);
#else
    const bool bilateral_asymmetry = false;
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    const bool send_compatible =
        this->video_source_ != nullptr && local_send.valid() && jpeg &&
        local_send.is_jpeg();
    const bool receive_compatible =
        this->video_sink_ != nullptr && local_receive.valid() && jpeg &&
        local_receive.is_jpeg();
#else
    const bool send_compatible =
        this->video_source_ != nullptr && local_send.valid() && h264 &&
        local_send.is_h264() &&
        h264_level_fits(local_send.profile_level_id, peer_profile);
    const bool receive_compatible =
        this->video_sink_ != nullptr && local_receive.valid() && h264 &&
        local_receive.is_h264() &&
        h264_same_subprofile(local_receive.profile_level_id, peer_profile);
#endif
    bool local_sends =
        this->video_send_requested_.load(std::memory_order_acquire) &&
        peer_receives && send_compatible;
    const bool local_receives = peer_sends && receive_compatible;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    if (!bilateral_asymmetry && local_sends && local_receives &&
        !h264_level_fits(local_send.profile_level_id,
                         local_receive.profile_level_id))
      local_sends = false;
#endif
    const bool inactive_direction = media_direction == 0;
    if (!local_sends && !local_receives &&
        (!inactive_direction || (!send_compatible && !receive_compatible)))
      continue;

    // Keep the shared RTP capability stable across direction-only re-INVITEs.
    // Endpoint dimensions remain directional local metadata (RFC 2435 carries
    // JPEG frame dimensions in-band), so choose the same compatible endpoint
    // whether or not that direction is active in the current offer.
    VideoCapability negotiated =
        receive_compatible ? local_receive : local_send;
    if (send_compatible && receive_compatible) {
      negotiated.max_fps =
          std::min(local_send.max_fps, local_receive.max_fps);
      if (local_send.max_bitrate_bps != 0 &&
          (negotiated.max_bitrate_bps == 0 ||
           local_send.max_bitrate_bps < negotiated.max_bitrate_bps)) {
        negotiated.max_bitrate_bps = local_send.max_bitrate_bps;
      }
    }
    if (media_max_fps > 0)
      negotiated.max_fps = std::min(negotiated.max_fps, media_max_fps);
    negotiated.payload_type = static_cast<uint8_t>(pt);
    negotiated.level_asymmetry_allowed =
        h264 && bilateral_asymmetry;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    negotiated.rtcp_feedback_pli = rtcp_feedback_pli[pt];
    negotiated.rtcp_feedback_fir = rtcp_feedback_fir[pt];
#else
    negotiated.rtcp_feedback_pli = false;
    negotiated.rtcp_feedback_fir = false;
#endif
    if (media_max_bitrate_bps != 0 &&
        (negotiated.max_bitrate_bps == 0 ||
         media_max_bitrate_bps < negotiated.max_bitrate_bps)) {
      negotiated.max_bitrate_bps = media_max_bitrate_bps;
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    if (!bilateral_asymmetry &&
        h264_level_fits(peer_profile, negotiated.profile_level_id)) {
      // Lowering a decoder advertisement is safe; lowering the encoder SPS is
      // not. Preserve our profile_idc/profile_iop and select the common level.
      negotiated.profile_level_id.replace(4, 2, peer_profile.substr(4, 2));
    }
#endif
    this->negotiated_video_capability_ = negotiated;
    this->remote_video_ip_v4_ = media_ip;
    this->remote_video_rtp_port_ = media_port;
    this->remote_video_rtcp_ip_v4_ = media_rtcp_ip;
    this->remote_video_rtcp_port_ = media_rtcp_port;
    this->video_send_enabled_ = local_sends;
    this->video_receive_enabled_ = local_receives;
    this->video_negotiated_ = true;
    ESP_LOGI(TAG,
             "SIP SDP selected video PT=%u %s remote=%08x:%u dir=%s%s",
             (unsigned) pt, negotiated.encoding.c_str(),
             (unsigned) media_ip, (unsigned) media_port,
             local_sends ? "send" : "", local_receives ? "recv" : "");
    return true;
  }

  return false;
}
#endif

}  // namespace voip_stack
}  // namespace esphome

#endif
