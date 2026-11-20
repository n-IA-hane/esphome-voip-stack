#include "sip_transport.h"
#include "sip_message.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

#include "sip_sdp_audio_negotiation.h"
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
#include "sip_sdp_video_selection.h"
#endif

#include <algorithm>
#include <cstring>
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
#include "sdp_imageattr.h"
#endif

#include <lwip/sockets.h>

#include "esphome/core/log.h"

namespace esphome {
namespace voip_stack {

static const char *const TAG = "voip_stack.sip";

std::string SipTransport::wrap_sdp_envelope_(const std::string &local_ip, const std::string &payloads,
                                             const std::string &maps, const std::string &flows,
                                             const AudioSdpFacts &facts, uint8_t ptime, const char *direction) const {
  return this->wrap_sdp_envelope_version_(local_ip, payloads, maps, flows, facts, ptime, direction,
      {this->sdp_session_id_, this->sdp_session_version_, this->rtp_port_});
}

std::string SipTransport::wrap_sdp_envelope_version_(const std::string &local_ip, const std::string &payloads,
                                             const std::string &maps, const std::string &flows,
                                             const AudioSdpFacts &facts, uint8_t ptime, const char *direction,
                                                     const AudioSdpEnvelope &envelope) const {
  if (!facts.valid()) {
    ESP_LOGW(TAG, "SIP SDP receive mappings have no declared packet time within the shared maxptime");
    return "";
  }
  const uint16_t maxptime = facts.maxptime;
  // ptime is a preference, but must not recommend packets above our bound.
  if (maxptime != 0 && ptime > maxptime) ptime = 0;
  return "v=0\r\n"
         "o=- " + std::to_string(envelope.session_id) + " " +
         std::to_string(envelope.version) + " IN IP4 " +
         local_ip + "\r\n"
         "s=VoIP Stack\r\n"
         "c=IN IP4 " + local_ip + "\r\n"
         "t=0 0\r\n"
         "m=audio " + std::to_string(envelope.port) + " RTP/AVP " + payloads + "\r\n" +
         maps +
         flows +
         "a=x-voip-stack-independent-ptime:1\r\n" +
         (ptime == 0 ? std::string() : "a=ptime:" + std::to_string(ptime) + "\r\n") +
         (maxptime == 0 ? std::string() : "a=maxptime:" + std::to_string(maxptime) + "\r\n") +
         "a=" + std::string(direction) + "\r\n";
}

void SipTransport::capture_remote_media_shape_(const std::string &sdp) {
  this->remote_media_shape_ = this->parse_remote_media_shape_(sdp);
}

SipTransport::RemoteMediaShape SipTransport::parse_remote_media_shape_(const std::string &sdp) const {
  RemoteMediaShape shape;
  shape.clear();
  size_t pos = 0;
  while (pos < sdp.size()) {
    size_t end = sdp.find("\r\n", pos);
    if (end == std::string::npos) end = sdp.size();
    const std::string line = sdp.substr(pos, end - pos);
    if (line.rfind("m=", 0) == 0) {
      const int8_t index = shape.count;
      if (shape.append(line)) {
        if (shape.audio_index < 0) {
          bool payloads[128]{};
          uint16_t port = 0;
          if (parse_audio_media_line(line, &port, payloads))
            shape.audio_index = index;
        }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
        if (shape.video_index < 0) {
          bool payloads[128]{};
          uint16_t port = 0;
          if (parse_video_media_line(line, &port, payloads))
            shape.video_index = index;
        }
#endif
      }
    }
    if (end == sdp.size()) break;
    pos = end + 2;
  }
  return shape;
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
                                            bool answer, bool capabilities) const {
  return this->append_negotiated_video_sdp_(sdp, local_ip, answer, capabilities,
                                            this->negotiated_media_());
}

std::string SipTransport::append_negotiated_video_sdp_(const std::string &sdp,
                                            const std::string &local_ip,
                                            bool answer, bool capabilities, const NegotiatedMedia &media) const {
  if ((!capabilities && this->refer_context_active_) || (this->video_source_ == nullptr && this->video_sink_ == nullptr)) {
    return sdp;
  }
  if (answer && media.video_offered && !media.video_negotiated) {
    return sdp + "m=video 0 RTP/AVP " +
           std::to_string(media.video.payload_type) +
           "\r\n";
  }
  const VideoCapability local_send = this->local_video_send_capability_();
  const VideoCapability local_receive =
      this->local_video_receive_capability_();
  const bool offer_send =
      (capabilities || this->video_send_requested_.load(std::memory_order_acquire)) &&
      this->video_source_ != nullptr && local_send.valid();
  const bool offer_receive =
      this->video_sink_ != nullptr && local_receive.valid();
  // RFC 6184 profile-level-id in a unicast offer declares the highest level
  // the offerer can receive. With bilateral level asymmetry the answer
  // independently declares the answerer's receive level, allowing a P4
  // hardware encoder and software decoder to use different envelopes.
  VideoCapability capability =
      answer ? media.video
             : offer_receive ? local_receive : local_send;
  if (!capability.valid()) {
    ESP_LOGW(TAG, "Video SDP omitted: local capabilities are invalid");
    return sdp;
  }
  const bool send =
      answer ? media.video_send
             : offer_send &&
                   local_send.encoding == capability.encoding;
  const bool receive =
      answer ? media.video_receive
             : offer_receive &&
                   local_receive.encoding == capability.encoding;
  if (!send && !receive && !answer) return sdp;
  const char *direction =
      send && receive ? "sendrecv"
                      : send ? "sendonly" : receive ? "recvonly" : "inactive";
  std::string out = sdp;
  out += "m=video " + std::to_string(capabilities ? 0 : this->video_rtp_port_) +
         " RTP/AVP " + std::to_string(capability.payload_type) + "\r\n";
  out += "c=IN IP4 " + local_ip + "\r\n";
  if (capability.max_bitrate_bps != 0) {
    out += "b=TIAS:" + std::to_string(capability.max_bitrate_bps) + "\r\n";
  }
  out += "a=rtcp:" +
         std::to_string(capabilities ? 0 : static_cast<uint16_t>(this->video_rtp_port_ + 1)) +
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
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (capability.is_jpeg() && (send || receive) &&
      (capabilities || !(media.imageattr_state & IMAGEATTR_DISABLED)) &&
      (!answer || (media.imageattr_state & IMAGEATTR_PEER))) {
    out += "a=imageattr:26";
    if (send) {
      // A fixed camera promises only its actual output, never arbitrary sizes.
      out += " send [x=" + std::to_string(local_send.width) + ",y=" +
             std::to_string(local_send.height) + ",sar=1.0]";
    }
    if (receive) {
      out += " recv [x=[8:8:" + std::to_string(local_receive.width) +
             "],y=[8:8:" + std::to_string(local_receive.height) + "],sar=1.0,q=1.0]";
    }
    out += "\r\n";
  }
#endif
  // The peer's encoding recommendation must describe our receive budget,
  // independently of the rate our own camera can transmit.
  const uint8_t advertised_fps = receive ? local_receive.max_fps : capability.max_fps;
  out += "a=framerate:" + std::to_string(advertised_fps) + "\r\n";
  out += std::string("a=") + direction + "\r\n";
  return out;
}
#endif

std::string SipTransport::build_sdp_capabilities_(uint32_t peer_ip) const {
  // OPTIONS describes configured endpoints without touching offer maps,
  // dialog generations, RTP reservations or the running media session.
  AudioFormatList formats;
  formats.count = 0;
  std::array<uint8_t, VOIP_STACK_MAX_AUDIO_FORMATS> directions{};
  const auto same_encoding = [](const AudioFormat &left, AudioFormat right) {
    // rtpmap describes the encoding; ptime is an independent optional hint.
    right.frame_ms = left.frame_ms;
    return audio_formats_share_wire_encoding(left, right);
  };
  const auto collect = [&](const AudioFormatList &input, uint8_t direction) {
    for (uint8_t i = 0; i < input.count; ++i) {
      const auto &format = input.formats[i];
      if (audio_format_rtp_encoding(format, this->udp_max_payload_) == nullptr) continue;
      uint8_t slot = 0;
      while (slot < formats.count && !same_encoding(formats.formats[slot], format)) ++slot;
      if (slot == formats.count) {
        if (formats.count == VOIP_STACK_MAX_AUDIO_FORMATS) continue;
        formats.formats[formats.count++] = format;
      }
      directions[slot] |= direction;
    }
  };
  collect(this->offer_rx_formats_, 2);
  collect(this->offer_tx_formats_, 1);
  if (formats.count == 0) return "";
  std::string payloads, maps, flows;
  AudioSdpFacts facts;
  for (uint8_t i = 0; i < formats.count && i < 24; ++i) {
    const auto &format = formats.formats[i];
    if (directions[i] & 2) facts.include_receive(format, this->offer_rx_formats_, this->udp_max_payload_);
    const uint8_t pt = format.codec == AudioCodec::G722 ? 9 : 96 + i;
    if (!payloads.empty()) payloads.push_back(' ');
    payloads += std::to_string(pt);
    maps += "a=rtpmap:" + std::to_string(pt) + " " + audio_format_rtp_encoding(format, this->udp_max_payload_) +
            "/" + std::to_string(audio_format_rtp_clock_rate(format)) + "/" +
            std::to_string(audio_format_rtp_channels(format)) + "\r\n";
    if (format.codec == AudioCodec::OPUS)
      maps += "a=fmtp:" + std::to_string(pt) + " stereo=0;sprop-stereo=0;maxaveragebitrate=28000\r\n";
    const char *flow = directions[i] == 3 ? "sendrecv" : directions[i] == 1 ? "send" : "recv";
    flows += "a=x-voip-stack-flow:" + std::to_string(pt) + " " + flow + "\r\n";
  }

  // Capability catalogs retain each direction's ordered packet-time choices.
  // The stream ptime below describes RX only and cannot describe TX choices.
  const auto capability_tokens = [&](const AudioFormatList &input) {
    std::string tokens;
    for (uint8_t i = 0; i < input.count; ++i) {
      const auto &format = input.formats[i];
      const char *encoding = audio_format_rtp_encoding(format, this->udp_max_payload_);
      if (!format.is_valid() || encoding == nullptr) continue;
      if (!tokens.empty()) tokens += ',';
      tokens += std::string(encoding) + "/" + std::to_string(audio_format_rtp_clock_rate(format)) +
                "/" + std::to_string(audio_format_rtp_channels(format)) + "/" + std::to_string(format.frame_ms);
    }
    return tokens;
  };
  const auto tx_capabilities = capability_tokens(this->offer_tx_formats_);
  const auto rx_capabilities = capability_tokens(this->offer_rx_formats_);
  if (!tx_capabilities.empty()) flows += "a=x-voip-stack-audio-tx:" + tx_capabilities + "\r\n";
  if (!rx_capabilities.empty()) flows += "a=x-voip-stack-audio-rx:" + rx_capabilities + "\r\n";

#ifdef USE_ESPHOME_VOIP_DTMF
  uint8_t clocks = 1;
  for (uint8_t i = 0; i < formats.count; ++i) {
    const int index = dtmf_clock_index(audio_format_rtp_clock_rate(formats.formats[i]));
    if (index >= 0) clocks |= 1U << index;
  }
  append_dtmf_sdp_(&payloads, &maps, clocks);
#endif
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(peer_ip, &local_ip);
  uint8_t ptime = preferred_audio_ptime(this->offer_rx_formats_, this->udp_max_payload_);
  // A capability union may contain codecs with different packet-time sets.
  // Omit the hint unless every described direction can use that one value.
  const auto supports_time = [&](const AudioFormatList &list, const AudioFormat &format) {
    for (uint8_t i = 0; i < list.count; ++i)
      if (list.formats[i].frame_ms == ptime && same_encoding(list.formats[i], format)) return true;
    return false;
  };
  for (uint8_t i = 0; i < formats.count && ptime != 0; ++i)
    if ((directions[i] & 2) && !supports_time(this->offer_rx_formats_, formats.formats[i])) ptime = 0;
  // RFC 3264 section 9 gives capabilities their own origin without changing
  // the live dialog. Serialize that identity and disabled media port directly.
  static std::atomic<uint32_t> sequence{0};
  const uint64_t session = (static_cast<uint64_t>(esp_random()) << 32) |
      sequence.fetch_add(1, std::memory_order_relaxed);
  std::string result = this->wrap_sdp_envelope_version_(local_ip, payloads, maps, flows, facts, ptime,
      audio_direction_attribute(this->offer_tx_formats_.count != 0, this->offer_rx_formats_.count != 0),
      {session, 0, 0});
  if (result.empty()) return "";

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  result = this->append_video_sdp_(result, local_ip, false, true);
#endif
  return result;
}

std::string SipTransport::build_sdp_offer_() {
  this->local_offered_rx_payloads_.count = 0;
  this->local_offered_tx_formats_.count = 0;
  this->local_offered_directional_audio_v1_ = this->peer_directional_audio_v1_;
  const uint32_t remote_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(remote_ip, &local_ip);
  std::string payloads;
  std::string maps;
  std::string flows;
  uint8_t pt = 96;
  AudioSdpFacts facts;
  const AudioFormat *retained_tx = this->offer_tx_formats_.count != 0 && this->refer_context_active_ && this->refer_dialog_
      ? &this->refer_dialog_->media.tx : nullptr;
  uint8_t selected_ptime = preferred_audio_ptime(
      this->offer_rx_formats_.count != 0 ? this->offer_rx_formats_ : this->offer_tx_formats_, this->udp_max_payload_);
  if (selected_ptime == 0) {
    ESP_LOGW(TAG, "SIP SDP offer has no RTP packet time for its active directions");
    return "";
  }
  AudioFormat offered_mappings[VOIP_STACK_MAX_AUDIO_FORMATS * 2]{};
  uint8_t offered_count = 0;
  auto append_format = [&](const AudioFormat &fmt, const char *flow) {
    if (pt >= 120) return;
    for (uint8_t i = 0; i < offered_count; ++i)
      if (audio_formats_share_rtp_mapping(offered_mappings[i], fmt)) return;
    // A REFER handoff retains the running transmitter until the new dialog commits.
    // Its sendrecv offer must not let the answer select a receive-only format.
    if (retained_tx != nullptr && !audio_formats_share_rtp_mapping(fmt, *retained_tx)) return;
    const char *enc = audio_format_rtp_encoding(fmt, this->udp_max_payload_);
    if (enc == nullptr) return;
    const uint8_t payload_pt = fmt.codec == AudioCodec::G722 ? 9 : pt;
    offered_mappings[offered_count++] = fmt;
    if (!payloads.empty()) payloads.push_back(' ');
    payloads += std::to_string(payload_pt);
    maps += "a=rtpmap:" + std::to_string(payload_pt) + " " + enc + "/" +
            std::to_string(audio_format_rtp_clock_rate(fmt)) + "/" +
            std::to_string(audio_format_rtp_channels(fmt)) + "\r\n";
    if (fmt.codec == AudioCodec::OPUS)
      maps += "a=fmtp:" + std::to_string(payload_pt) +
              " stereo=0;sprop-stereo=0;maxaveragebitrate=28000\r\n";
    AudioFormat local_format;
    if (audio_format_list_match_rtp_mapping(this->offer_rx_formats_, fmt, 0, &local_format, this->udp_max_payload_))
      this->local_offered_rx_payloads_.add(payload_pt, local_format);
    if (audio_format_list_match_rtp_mapping(this->offer_tx_formats_, fmt, 0, &local_format, this->udp_max_payload_) &&
        this->local_offered_tx_formats_.count < VOIP_STACK_MAX_AUDIO_FORMATS)
      this->local_offered_tx_formats_.formats[this->local_offered_tx_formats_.count++] = local_format;
    if (this->peer_directional_audio_v1_)
      flows += "a=x-voip-stack-flow:" + std::to_string(payload_pt) + " " + flow + "\r\n";
    if (this->offer_rx_formats_.count != 0 &&
        (!this->peer_directional_audio_v1_ || std::strcmp(flow, "send") != 0))
      facts.include_receive(fmt, this->offer_rx_formats_, this->udp_max_payload_);
    pt++;
  };
  for (uint8_t i = 0; i < this->offer_rx_formats_.count && pt < 120; i++) {
    bool shared = false;
    for (uint8_t j = 0; j < this->offer_tx_formats_.count; j++)
      if (audio_formats_share_rtp_mapping(this->offer_tx_formats_.formats[j],
                                          this->offer_rx_formats_.formats[i])) {
        shared = true;
        break;
      }
    if (shared || this->offer_tx_formats_.count == 0 || this->peer_directional_audio_v1_)
      append_format(this->offer_rx_formats_.formats[i], shared ? "sendrecv" : "recv");
  }
  for (uint8_t i = 0; (this->peer_directional_audio_v1_ || this->offer_rx_formats_.count == 0) &&
                      i < this->offer_tx_formats_.count && pt < 120; i++) {
    bool already_offered = false;
    for (uint8_t j = 0; j < this->offer_rx_formats_.count; j++)
      if (audio_formats_share_rtp_mapping(this->offer_rx_formats_.formats[j],
                                          this->offer_tx_formats_.formats[i])) already_offered = true;
    if (already_offered) continue;
    append_format(this->offer_tx_formats_.formats[i], "send");
  }
  if (payloads.empty() || (this->offer_tx_formats_.count != 0 && this->local_offered_tx_formats_.count == 0)) {
    ESP_LOGW(TAG, "SIP SDP offer has no common UDP-safe RTP PCM format");
    return "";
  }
  // Advertise the preference of an actual offered receiver, rather than a
  // receive-only mapping omitted from an ordinary sendrecv offer.
  selected_ptime = this->local_offered_rx_payloads_.count != 0
      ? this->local_offered_rx_payloads_.entries[0].format.frame_ms
      : this->local_offered_tx_formats_.formats[0].frame_ms;
#ifdef USE_ESPHOME_VOIP_DTMF
  this->dtmf_offered_clocks_ = 1;
  for (const auto *list : {&this->local_offered_tx_formats_, &this->offer_rx_formats_})
    for (uint8_t i = 0; i < list->count; ++i) {
      const int index = dtmf_clock_index(audio_format_rtp_clock_rate(list->formats[i]));
      if (index >= 0) this->dtmf_offered_clocks_ |= 1U << index;
    }
  append_dtmf_sdp_(&payloads, &maps, this->dtmf_offered_clocks_);
#endif
  std::string sdp =
      this->wrap_sdp_envelope_(local_ip, payloads, maps, flows, facts, selected_ptime,
          audio_direction_attribute(this->offer_tx_formats_.count != 0, this->offer_rx_formats_.count != 0));
  if (sdp.empty()) return "";
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  sdp = this->append_video_sdp_(sdp, local_ip, false);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (sdp.find("\r\na=imageattr:26 ") != std::string::npos)
    this->video_imageattr_state_ |= IMAGEATTR_OFFERED;
  else
    this->video_imageattr_state_ &= ~IMAGEATTR_OFFERED;
#endif
#endif
  return sdp;
}

std::string SipTransport::build_sdp_answer_(
#ifdef USE_ESPHOME_VOIP_DTMF
    const DtmfConfig *dtmf_proposal,
#endif
    const AudioSdpDirection *audio_direction) const {
  auto media = this->negotiated_media_();
#ifdef USE_ESPHOME_VOIP_DTMF
  if (dtmf_proposal != nullptr) media.dtmf = *dtmf_proposal;
#endif
  if (audio_direction != nullptr) {
    media.send = audio_direction->send;
    media.receive = audio_direction->receive;
  }
  return this->build_negotiated_sdp_answer_(media, this->sdp_session_version_);
}

std::string SipTransport::build_negotiated_sdp_answer_(const NegotiatedMedia &media,
                                                            uint32_t version) const {
  const uint32_t remote_ip = this->remote_ip_v4_.load(std::memory_order_acquire);
  std::string local_ip = "0.0.0.0";
  this->local_ip_for_peer_(remote_ip, &local_ip);
  AudioFormat selected_tx;
  AudioFormat selected_rx;
  uint8_t tx_payload_type = 96;
  uint8_t rx_payload_type = 96;
  RtpAudioPayloads rx_payloads;
  selected_tx = media.tx; selected_rx = media.rx;
  tx_payload_type = media.tx_pt; rx_payload_type = media.rx_pt; rx_payloads = media.rx_payloads;
  const bool have_tx = selected_tx.is_valid();
  const bool have_rx = selected_rx.is_valid();
  const bool send = have_tx && media.send;
  const bool receive = have_rx && media.receive;
  if (!have_tx && !have_rx) return "";
  // RFC 3264 section 6.1: one-way answers describe only that direction's
  // formats. An inactive answer retains the sendrecv codec description.
  const bool inactive = !send && !receive;
  const bool advertise_tx = send || (inactive && have_tx);
  const bool advertise_rx = receive || (inactive && have_rx);
  std::string payloads, maps, flows;
  AudioSdpFacts facts;
  auto append = [&](uint8_t pt, const AudioFormat &format, const char *flow) {
    const char *encoding = audio_format_rtp_encoding(format, this->udp_max_payload_);
    if (encoding == nullptr) return false;
    if (!payloads.empty()) payloads += " ";
    payloads += std::to_string(pt);
    maps += "a=rtpmap:" + std::to_string(pt) + " " + encoding + "/" +
        std::to_string(audio_format_rtp_clock_rate(format)) + "/" +
        std::to_string(audio_format_rtp_channels(format)) + "\r\n";
    if (format.codec == AudioCodec::OPUS)
      maps += "a=fmtp:" + std::to_string(pt) +
          " stereo=0;sprop-stereo=0;maxaveragebitrate=28000\r\n";
    if (media.directional_audio)
      flows += "a=x-voip-stack-flow:" + std::to_string(pt) + " " + flow + "\r\n";
    if (advertise_rx && (!media.directional_audio || std::strcmp(flow, "send") != 0))
      facts.include_receive(format, this->offer_rx_formats_, this->udp_max_payload_);
    return true;
  };
  if (advertise_rx && !append(rx_payload_type, selected_rx,
                        advertise_tx && tx_payload_type == rx_payload_type ? "sendrecv" : "recv")) return "";
  for (uint8_t i = 0; advertise_rx && i < rx_payloads.count; ++i) {
    const auto &entry = rx_payloads.entries[i];
    if (entry.payload_type == rx_payload_type || (advertise_tx && entry.payload_type == tx_payload_type)) continue;
    if (!append(entry.payload_type, entry.format, "recv")) return "";
  }
  if (advertise_tx && (!advertise_rx || tx_payload_type != rx_payload_type) &&
      !append(tx_payload_type, selected_tx,
              advertise_rx && rx_payloads.find(tx_payload_type, nullptr) ? "sendrecv" : "send")) return "";
#ifdef USE_ESPHOME_VOIP_DTMF
  const DtmfFormat dtmf = media.dtmf.rx.valid() ? media.dtmf.rx : media.dtmf.tx;
  if (dtmf.valid()) {
    const auto pt = std::to_string(dtmf.payload_type);
    payloads += " " + pt;
    maps += "a=rtpmap:" + pt + " telephone-event/" + std::to_string(dtmf.clock) + "\r\n";
    std::string events;
    for (unsigned event = 0; event < 16; ++event) if (dtmf.events & (1U << event)) {
      if (!events.empty()) events += ',';
      events += std::to_string(event);
    }
    maps += "a=fmtp:" + pt + " " + events + "\r\n";
  }
#endif
  const std::string audio_sdp = this->wrap_sdp_envelope_version_(
      local_ip, payloads, maps, flows, facts, advertise_rx ? selected_rx.frame_ms : selected_tx.frame_ms,
      audio_direction_attribute(send, receive), {this->sdp_session_id_, version, this->rtp_port_});
  const size_t audio_media = audio_sdp.find("m=audio ");
  if (audio_media == std::string::npos) return "";
  const std::string session = audio_sdp.substr(0, audio_media);
  const std::string audio = audio_sdp.substr(audio_media);
  std::string video;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  video = this->append_negotiated_video_sdp_("", local_ip, true, false, media);
#endif
  if (media.shape.empty()) return audio_sdp + video;
  if (media.shape.overflow) {
    ESP_LOGW(TAG, "SIP SDP answer refused: more than %u media lines",
             (unsigned) kMaxSdpMediaLines);
    return "";
  }

  // RFC 3264 section 6 requires exactly one answer m-line for every offered
  // m-line, in the same order. Unsupported and duplicate streams are retained
  // with port zero instead of being silently omitted or reordered.
  std::string answer = session;
  for (size_t index = 0; index < media.shape.count; index++) {
    if (static_cast<int>(index) == media.shape.audio_index) {
      answer += audio;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    } else if (static_cast<int>(index) == media.shape.video_index &&
               !video.empty()) {
      answer += video;
#endif
    } else {
      const std::string rejected = this->rejected_media_answer_(
          media.shape.line(index));
      if (rejected.empty()) return "";
      answer += rejected;
    }
  }
  return answer;
}

bool SipTransport::learn_remote_rtp_from_sdp_(const std::string &sdp,
                                              uint32_t default_ip,
                                              bool remote_is_answer
#ifdef USE_ESPHOME_VOIP_DTMF
                                              , DtmfConfig *dtmf_proposal
#endif
                                              , AudioSdpDirection *audio_direction) {
  const auto retained = this->negotiated_media_();
  NegotiatedMedia media;
  if (!this->parse_remote_media_(sdp, default_ip, remote_is_answer, retained, &media,
                                  audio_direction != nullptr)) return false;
#ifdef USE_ESPHOME_VOIP_DTMF
  if (dtmf_proposal != nullptr) {
    *dtmf_proposal = media.dtmf;
    media.dtmf = retained.dtmf;
  }
#endif
  if (audio_direction != nullptr) {
    audio_direction->send = media.send;
    audio_direction->receive = media.receive;
    audio_direction->zero_connection = media.zero_connection;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
    audio_direction->video_zero_connection = media.video_zero_connection;
#endif
  }
  this->commit_negotiated_media_(media);
  return true;
}

bool SipTransport::parse_remote_media_(const std::string &sdp, uint32_t default_ip,
                                        bool remote_is_answer, const NegotiatedMedia &retained,
                                        NegotiatedMedia *media, bool retained_call) const {
  *media = NegotiatedMedia{};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  media->imageattr_state = retained.imageattr_state;
#endif
  media->shape = this->parse_remote_media_shape_(sdp);
  if (media->shape.overflow) {
    ESP_LOGW(TAG, "SIP SDP rejected: too many media lines");
    return false;
  }
  const LocalAudioOffer local{this->offer_tx_formats_, this->offer_rx_formats_,
                             this->local_offered_tx_formats_, this->local_offered_rx_payloads_,
                             this->udp_max_payload_, this->local_offered_directional_audio_v1_
#ifdef USE_ESPHOME_VOIP_DTMF
                             , this->dtmf_offered_clocks_
#endif
  };
  const auto report_mapping = []([[maybe_unused]] uint8_t pt, [[maybe_unused]] const AudioFormat &fmt,
                                 [[maybe_unused]] const AudioPayloadSelection &selection,
                                 const AudioMappingChanges &changes) {
    if (changes.rx) {
      ESP_LOGI(TAG, "SIP SDP selected RX PT=%u %s/%u/%u frame=%ums",
               (unsigned) pt, audio_format_rtp_encoding(selection.selected_rx_format),
               (unsigned) audio_format_rtp_clock_rate(selection.selected_rx_format),
               (unsigned) audio_format_rtp_channels(selection.selected_rx_format),
               (unsigned) selection.selected_rx_format.frame_ms);
    }
    if (changes.tx) {
      ESP_LOGI(TAG, "SIP SDP selected TX PT=%u %s/%u/%u frame=%ums",
               (unsigned) pt, audio_format_rtp_encoding(selection.selected_tx_format),
               (unsigned) audio_format_rtp_clock_rate(selection.selected_tx_format),
               (unsigned) audio_format_rtp_channels(selection.selected_tx_format),
               (unsigned) selection.selected_tx_format.frame_ms);
    } else if (changes.unsupported) {
      ESP_LOGD(TAG, "SIP SDP skipping unsupported PT=%u rate=%u pcm=%u channels=%u",
               (unsigned) pt, (unsigned) fmt.sample_rate, (unsigned) fmt.pcm_format, (unsigned) fmt.channels);
    }
  };
  const auto report_incompatible = []([[maybe_unused]] size_t body_size, [[maybe_unused]] uint16_t port,
                                      [[maybe_unused]] uint32_t media_ip, [[maybe_unused]] bool tx,
                                      [[maybe_unused]] bool rx) {
    ESP_LOGW(TAG,
             "SIP SDP rejected: body_len=%u media_port=%u media_ip=%08x "
             "selected_tx=%s selected_rx=%s",
             (unsigned) body_size, (unsigned) port, (unsigned) media_ip, tx ? "yes" : "no",
             rx ? "yes" : "no");
  };
  if (!negotiate_remote_audio(sdp, default_ip, remote_is_answer, retained_call, local, retained, *media,
#ifdef USE_ESPHOME_VOIP_DTMF
                              media->dtmf,
#endif
                              report_mapping, report_incompatible)) return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (!this->refer_context_active_ && !this->parse_remote_video_(sdp, default_ip, remote_is_answer, retained, media, retained_call)) {
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
                                                bool remote_is_answer,
                                                bool *zero_connection) {
  const auto retained = this->negotiated_media_();
  auto media = retained;
  const bool valid = this->parse_remote_video_(sdp, default_ip, remote_is_answer, retained,
                                               &media, zero_connection != nullptr);
  if (zero_connection != nullptr) *zero_connection = media.video_zero_connection;
  this->commit_negotiated_media_(media);
  return valid;
}

bool SipTransport::parse_remote_video_(const std::string &sdp, uint32_t default_ip,
                                        bool remote_is_answer, const NegotiatedMedia &retained,
                                        NegotiatedMedia *media, bool allow_zero_connection) const {
  const uint32_t retained_ip = retained.video_ip;
  const uint32_t retained_rtcp_ip = retained.video_rtcp_ip;
  media->video_offered = false; media->video_negotiated = false;
  media->video_send = false; media->video_receive = false;
  media->video_ip = 0; media->video_port = 0;
  media->video_rtcp_ip = 0; media->video_rtcp_port = 0;
  media->video = {}; media->video_zero_connection = false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  media->imageattr_state &= ~IMAGEATTR_PEER;
#endif
  const VideoCapability local_send = this->local_video_send_capability_();
  const VideoCapability local_receive = this->local_video_receive_capability_();
  VideoOfferAttributes offer;
  read_video_offer_attributes(sdp, default_ip, local_send, local_receive, offer);
  media->video_offered = offer.offered;
  const auto disposition = validate_video_offer(offer, retained_ip, retained_rtcp_ip, allow_zero_connection,
                                                media->video.payload_type, media->video_zero_connection);
  if (disposition != VideoOfferDisposition::SELECT_PAYLOAD)
    return disposition == VideoOfferDisposition::DISABLED;
  VideoPayloadSelection selected{media->video};
  if (!select_video_payload(offer, local_send, local_receive, this->video_send_requested_.load(std::memory_order_acquire),
                            remote_is_answer, this->video_offer_payload_type_, selected))
    return false;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (selected.imageattr_present) {
    if (selected.imageattr_compatible) {
      media->imageattr_state |= IMAGEATTR_PEER;
    } else if (remote_is_answer && (media->imageattr_state & IMAGEATTR_OFFERED) &&
               !(media->imageattr_state & IMAGEATTR_DISABLED)) {
      media->imageattr_state |= IMAGEATTR_DISABLED | IMAGEATTR_FALLBACK;
    }
  }
#endif
  media->video_ip = offer.media_ip;
  media->video_port = offer.media_port;
  media->video_rtcp_ip = offer.media_rtcp_ip;
  media->video_rtcp_port = offer.media_rtcp_port;
  media->video_send = selected.send;
  media->video_receive = selected.receive;
  media->video_negotiated = true;
  ESP_LOGI(TAG, "SIP SDP selected video PT=%u %s remote=%08x:%u dir=%s%s",
           (unsigned) media->video.payload_type, media->video.encoding.c_str(),
           (unsigned) offer.media_ip, (unsigned) offer.media_port,
           selected.send ? "send" : "", selected.receive ? "recv" : "");
  return true;
}
#endif

}  // namespace voip_stack
}  // namespace esphome

#endif
