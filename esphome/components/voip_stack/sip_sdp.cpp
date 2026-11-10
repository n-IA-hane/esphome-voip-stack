#include "sip_transport.h"
#include "sip_message.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

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
  media->directional_audio = false;
  media->shape = this->parse_remote_media_shape_(sdp);
  if (media->shape.overflow) {
    ESP_LOGW(TAG, "SIP SDP rejected: too many media lines");
    return false;
  }
  uint16_t media_port = 0;
  uint32_t media_ip = default_ip;
  bool zero_connection = false;
  uint8_t media_ptime = 0; // Missing ptime leaves the explicit local TX preference available.
  uint32_t media_maxptime = 0;
  bool has_maxptime = false;
  bool selected_tx = false;
  bool selected_rx = false;
  bool shared_format = false;
  RtpAudioPayloads received_payloads;
  AudioFormat selected_tx_format = disabled_audio_format();
  AudioFormat selected_rx_format = disabled_audio_format();
  uint8_t selected_tx_payload_type = 0;
  uint8_t selected_rx_payload_type = 0;
  uint8_t payload_flow[128]{};
  bool offered_payload[128]{};
  uint8_t payload_rank[128];
  std::fill(payload_rank, payload_rank + 128, 255);
  uint8_t selected_tx_rank = 255;
  uint32_t seen_mapping[128]{};
  uint8_t session_flow = 0x03;
  uint8_t media_flow = session_flow;
  size_t selected_audio_line = std::string::npos;
  bool seen_any_media = false;
  bool in_audio = false;
  bool independent_audio_ptime = false;
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
          // The m-line, not the order of rtpmap attributes, owns preference.
          size_t cursor = line.find("RTP/AVP") + 7;
          uint8_t rank = 0;
          while ((cursor = line.find_first_not_of(" \t", cursor)) != std::string::npos) {
            const size_t last = line.find_first_of(" \t", cursor);
            uint32_t value = 0;
            if (!parse_decimal_u32(line.substr(cursor, last - cursor), 127, &value)) return false;
            if (payload_rank[value] == 255) payload_rank[value] = rank++;
            if (last == std::string::npos) break;
            cursor = last;
          }
          media_port = candidate_port;
          std::copy(candidate_payload, candidate_payload + 128, offered_payload);
          selected_audio_line = ptime_pos;
          media_flow = session_flow;
          in_audio = true;
        }
      }
    } else if (in_audio && line == "a=x-voip-stack-independent-ptime:1") {
      independent_audio_ptime = true;
    } else if (in_audio && line.rfind("a=ptime:", 0) == 0) {
      uint32_t parsed = 0;
      if (parse_decimal_u32(line.substr(8), UINT8_MAX, &parsed) &&
          (parsed == 10 || parsed == 16 || parsed == 20 || parsed == 32)) {
        media_ptime = static_cast<uint8_t>(parsed);
      }
    } else if (in_audio && line.rfind("a=maxptime:", 0) == 0) {
      // RFC 8866 allows a nonzero integer or decimal. Durations in our
      // explicit format list are integral, so flooring the bound is exact.
      const auto value = line.substr(11);
      const auto dot = value.find('.');
      uint32_t whole = 0;
      if (!parse_decimal_u32(value.substr(0, dot), UINT32_MAX, &whole)) return false;
      bool fraction_nonzero = false;
      if (dot != std::string::npos) {
        if (dot + 1 == value.size()) return false;
        for (size_t i = dot + 1; i < value.size(); ++i) {
          if (value[i] < '0' || value[i] > '9') return false;
          fraction_nonzero |= value[i] != '0';
        }
      }
      if (whole == 0 && !fraction_nonzero) return false;
      media_maxptime = has_maxptime ? std::min(media_maxptime, whole) : whole;
      has_maxptime = true;
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
  for (uint8_t pt = 0; pt < 128; ++pt)
    if (offered_payload[pt] && payload_flow[pt] != 0 &&
        (!remote_is_answer || this->local_offered_directional_audio_v1_))
      media->directional_audio = true;
  // Only the earlier private D1 contract coupled both directions. Ordinary
  // SIP and peers explicitly declaring this version use RFC 3264 semantics.
  const bool legacy_directional_ptime = media->directional_audio && !independent_audio_ptime;
  const uint16_t transmit_ptime = legacy_directional_ptime && media_ptime == 0
      ? choose_common_audio_ptime(this->offer_tx_formats_, this->offer_rx_formats_, this->udp_max_payload_)
      : media_ptime;
  if (legacy_directional_ptime && transmit_ptime == 0) return false;
  AudioFormat retained_tx_format = disabled_audio_format();
  if (retained_call && !remote_is_answer && media_ptime == 0)
    retained_tx_format = retained.tx;
  auto consider_mapping = [&](AudioFormat fmt, uint8_t pt) {
    if (pt == 9 && fmt.codec != AudioCodec::G722) return false;
    fmt.frame_ms = media_ptime;
    const uint32_t packed_mapping = pack_audio_format(fmt);
    if (seen_mapping[pt] != 0 && seen_mapping[pt] != packed_mapping) return false;
    seen_mapping[pt] = packed_mapping;
    AudioFormat local_rx;
    AudioFormat local_tx;
    // Preserve the prepared codec geometry during a direction-only proposal.
    // D1 payload capabilities remain directional; the m-line gates activity.
    const uint8_t flow = !media->directional_audio || payload_flow[pt] == 0
                             ? (retained_call ? 0x03 : media_flow) : payload_flow[pt];
    const bool peer_can_send = (flow & 0x01) != 0;
    const bool peer_can_recv = (flow & 0x02) != 0;
    const auto match_tx = [&](uint16_t preferred_time) {
      for (uint8_t i = 0; i < this->offer_tx_formats_.count; ++i) {
        const auto &candidate = this->offer_tx_formats_.formats[i];
        if ((preferred_time == 0 || candidate.frame_ms == preferred_time) &&
            (!has_maxptime || candidate.frame_ms <= media_maxptime) &&
            audio_formats_share_rtp_mapping(candidate, fmt) &&
            audio_format_rtp_encoding(candidate, this->udp_max_payload_) != nullptr) {
          local_tx = candidate;
          return true;
        }
      }
      return false;
    };
    uint16_t preferred_tx_ptime = transmit_ptime;
    // Omitting an optional preference in a direction-only reoffer must not
    // invent a TX quantum change. match_tx still enforces the declared
    // mapping, peer maxptime and UDP budget before retaining that duration.
    if (preferred_tx_ptime == 0 && retained_tx_format.is_valid() &&
        audio_formats_share_rtp_mapping(retained_tx_format, fmt))
      preferred_tx_ptime = retained_tx_format.frame_ms;
    bool tx_ok = peer_can_recv && (match_tx(preferred_tx_ptime) ||
        (!legacy_directional_ptime && preferred_tx_ptime != 0 && match_tx(0)));
    // The peer's ptime is its receive preference, hence our TX duration.
    // Our receive duration belongs to our own offer/answer, not that hint.
    const uint16_t offered_rx_ptime = legacy_directional_ptime ? transmit_ptime : remote_is_answer
        ? preferred_audio_ptime(this->offer_rx_formats_, this->udp_max_payload_) : 0;
    bool rx_ok = peer_can_send &&
                       (audio_format_list_match_rtp_mapping(this->offer_rx_formats_, fmt, offered_rx_ptime, &local_rx,
                                                            this->udp_max_payload_) ||
                        (!legacy_directional_ptime &&
                         audio_format_list_match_rtp_mapping(this->offer_rx_formats_, fmt, 0, &local_rx,
                                                             this->udp_max_payload_)));
    const auto local_rank = [](const AudioFormatList &list, const AudioFormat &format) {
      for (uint8_t i = 0; i < list.count; ++i) if (list.formats[i] == format) return i;
      return static_cast<uint8_t>(VOIP_STACK_MAX_AUDIO_FORMATS);
    };
    uint8_t receive_pt = pt;
    if (remote_is_answer) {
      // Configured duration and UDP budget were checked above. The answer
      // may request another ptime while retaining an offered RTP mapping.
      bool offered_tx = false;
      for (uint8_t i = 0; i < this->local_offered_tx_formats_.count; ++i) {
        if (audio_formats_share_rtp_mapping(this->local_offered_tx_formats_.formats[i], fmt)) {
          offered_tx = true;
          break;
        }
      }
      tx_ok = tx_ok && offered_tx;
      bool offered_rx = false;
      for (uint8_t i = 0; i < this->local_offered_rx_payloads_.count; ++i) {
        const auto &entry = this->local_offered_rx_payloads_.entries[i];
        if (audio_formats_share_rtp_mapping(entry.format, fmt)) {
          receive_pt = entry.payload_type;
          offered_rx = true;
          break;
        }
      }
      rx_ok = rx_ok && offered_rx;
    }
    shared_format |= tx_ok && rx_ok;
    bool alternate_pcm = false;
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
    alternate_pcm = local_rx.codec == AudioCodec::PCM && local_rx.pcm_format == PcmFormat::S16LE;
#endif
    const bool unidirectional = this->offer_tx_formats_.count == 0 || this->offer_rx_formats_.count == 0;
    const bool eligible = unidirectional || media->directional_audio || media_flow != 0x03 ||
                          (tx_ok && rx_ok) || alternate_pcm;
    // RFC 3264 allows independent send preferences. Keep every accepted
    // receive mapping; select the first compatible local preference.
    // Accepted lower S16 rates use the existing audio-library resampler.
    if (rx_ok && eligible && !received_payloads.add(receive_pt, local_rx)) return false;
    if (rx_ok && eligible && (!selected_rx ||
        local_rank(this->offer_rx_formats_, local_rx) < local_rank(this->offer_rx_formats_, selected_rx_format))) {
      selected_rx_format = local_rx;
      selected_rx_payload_type = receive_pt;
      selected_rx = true;
      ESP_LOGI(TAG, "SIP SDP selected RX PT=%u %s/%u/%u frame=%ums",
               (unsigned) pt, audio_format_rtp_encoding(selected_rx_format),
               (unsigned) audio_format_rtp_clock_rate(selected_rx_format),
               (unsigned) audio_format_rtp_channels(selected_rx_format),
               (unsigned) selected_rx_format.frame_ms);
    }
    if (tx_ok && eligible && (unidirectional || media->directional_audio || media_flow != 0x03 || rx_ok) &&
        (!selected_tx ||
         local_rank(this->offer_tx_formats_, local_tx) < local_rank(this->offer_tx_formats_, selected_tx_format) ||
         (local_rank(this->offer_tx_formats_, local_tx) == local_rank(this->offer_tx_formats_, selected_tx_format) &&
          payload_rank[pt] < selected_tx_rank))) {
      selected_tx_format = local_tx;
      selected_tx_payload_type = pt;
      selected_tx_rank = payload_rank[pt];
      selected_tx = true;
      ESP_LOGI(TAG, "SIP SDP selected TX PT=%u %s/%u/%u frame=%ums",
               (unsigned) pt, audio_format_rtp_encoding(selected_tx_format),
               (unsigned) audio_format_rtp_clock_rate(selected_tx_format),
               (unsigned) audio_format_rtp_channels(selected_tx_format),
               (unsigned) selected_tx_format.frame_ms);
    } else if (!selected_tx && !selected_rx) {
      ESP_LOGD(TAG, "SIP SDP skipping unsupported PT=%u rate=%u pcm=%u channels=%u",
               (unsigned) pt,
               (unsigned) fmt.sample_rate,
               (unsigned) fmt.pcm_format,
               (unsigned) fmt.channels);
    }

    return true;
  };
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
      if (inet_aton(line.substr(9).c_str(), &a) != 0) {
        zero_connection = a.s_addr == 0;
        if (!zero_connection) media_ip = ntohl(a.s_addr);
      }
    } else if (in_audio && line.rfind("a=rtpmap:", 0) == 0) {
      AudioFormat fmt;
      uint8_t pt = 0;
      const bool valid_mapping = parse_rtpmap_format(line, &fmt, &pt);
      uint32_t declared_pt = 0;
      const size_t separator = line.find_first_of(" \t", 9);
      if (offered_payload[9] && parse_decimal_u32(line.substr(9, separator - 9), 127, &declared_pt) &&
          declared_pt == 9 && !valid_mapping) return false;
      if (valid_mapping && offered_payload[pt]) {
        if (!consider_mapping(fmt, pt)) return false;
      }
    }
    if (end == sdp.size()) break;
    pos = end + 2;
  }
  if (offered_payload[9] && seen_mapping[9] == 0) {
    const AudioFormat g722{16000, PcmFormat::S16LE, 1, 20, AudioCodec::G722};
    if (!consider_mapping(g722, 9)) return false;
  }
  const bool want_tx = this->offer_tx_formats_.count != 0 &&
      !zero_connection && (media_flow & 0x02) != 0;
  const bool want_rx = this->offer_rx_formats_.count != 0 &&
      !zero_connection && (media_flow & 0x01) != 0;
  if (retained_call) {
    AudioFormat retained_tx, retained_rx;
    uint8_t retained_tx_pt = 0, retained_rx_pt = 0;
    retained_tx = retained.tx; retained_rx = retained.rx;
    retained_tx_pt = retained.tx_pt; retained_rx_pt = retained.rx_pt;
    const auto retained_mapping_offered = [&](const AudioFormat &format, uint8_t pt) {
      return format.is_valid() && seen_mapping[pt] != 0 &&
          audio_formats_share_rtp_mapping(format, unpack_audio_format(seen_mapping[pt]));
    };
    // A suspended direction need not appear in a one-way offer. Keep exactly
    // its admitted geometry/PT instead of inventing or migrating a mapping.
    if (!want_tx && !want_rx &&
        !retained_mapping_offered(retained_tx, retained_tx_pt) &&
        !retained_mapping_offered(retained_rx, retained_rx_pt)) return false;
    if (!want_tx) {
      selected_tx_format = retained_tx;
      selected_tx_payload_type = retained_tx_pt;
      selected_tx = retained_tx.is_valid();
    }
    if (!want_rx) {
      selected_rx_format = retained_rx;
      selected_rx_payload_type = retained_rx_pt;
      selected_rx = retained_rx.is_valid();
    }
  }
  if (media_port == 0 || media_ip == 0 ||
      (!want_tx && !want_rx && !retained_call) ||
      (!selected_tx && !selected_rx) ||
      (want_tx && !selected_tx) || (want_rx && !selected_rx) ||
      (want_tx && want_rx && !media->directional_audio && media_flow == 0x03 && !shared_format)) {
    ESP_LOGW(TAG,
             "SIP SDP rejected: body_len=%u media_port=%u media_ip=%08x "
             "selected_tx=%s selected_rx=%s",
             (unsigned) sdp.size(), (unsigned) media_port, (unsigned) media_ip, selected_tx ? "yes" : "no",
             selected_rx ? "yes" : "no");
    return false;
  }
  // Initial setup has no retained media owner to pause. Reject the legacy
  // address rather than acknowledging it while transmitting to the fallback.
  if (zero_connection && !retained_call) return false;
  media->zero_connection = zero_connection;
  media->send = !zero_connection && selected_tx && (media_flow & 0x02) != 0;
  media->receive = !zero_connection && selected_rx && (media_flow & 0x01) != 0;
  RtpAudioPayloads accepted_rx;
  for (uint8_t i = 0; i < received_payloads.count; ++i) {
    const auto &entry = received_payloads.entries[i];
    bool usable = entry.format == selected_rx_format;
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
    AudioFormat target_pcm = decoded_pcm_format(selected_rx_format);
    target_pcm.frame_ms = entry.format.frame_ms;
    // D1 keeps configured PCM alternatives supported by the existing receive
    // converter, while other codecs retain their directional geometry rule.
    usable |= (!media->directional_audio || entry.format.codec == AudioCodec::PCM) &&
              pcm_receive_conversion_supported(decoded_pcm_format(entry.format), target_pcm);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
    usable |= !media->directional_audio &&
              same_decoded_audio_geometry(entry.format, selected_rx_format);
#endif
    if (usable && !accepted_rx.add(entry.payload_type, entry.format)) return false;
  }
  if (retained_call && !media->receive) accepted_rx = retained.rx_payloads;
#ifdef USE_ESPHOME_VOIP_DTMF
  const auto next_media = sdp.find("\r\nm=", selected_audio_line);
  const std::string_view audio_section(sdp.data() + selected_audio_line,
      (next_media == std::string::npos ? sdp.size() : next_media + 2) - selected_audio_line);
  auto dtmf = negotiate_dtmf(audio_section, payload_rank,
      audio_format_rtp_clock_rate(selected_tx ? selected_tx_format : selected_rx_format),
      remote_is_answer, this->dtmf_offered_clocks_);
  if (dtmf.valid() && (seen_mapping[dtmf.payload_type] != 0 ||
      received_payloads.find(dtmf.payload_type, nullptr) ||
      (selected_tx && selected_tx_payload_type == dtmf.payload_type))) dtmf = {};
  DtmfConfig dtmf_config{selected_tx ? dtmf : DtmfFormat{}, selected_rx ? dtmf : DtmfFormat{}};
  if (remote_is_answer && dtmf_config.rx.valid()) dtmf_config.rx.payload_type = 120 + dtmf_clock_index(dtmf.clock);
  media->dtmf = dtmf_config;
#endif
  media->tx = selected_tx_format; media->rx = selected_rx_format;
  media->tx_pt = selected_tx_payload_type; media->rx_pt = selected_rx_payload_type;
  media->rx_payloads = accepted_rx;
  // Signaling can traverse a PBX/proxy while RTP terminates on a separate
  // media address from SDP. Never overwrite the SIP peer with the media peer.
  media->audio_ip = zero_connection ? retained.audio_ip : media_ip;
  media->audio_port = media_port;
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
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  ImageAttributeMatch imageattr_specific, imageattr_wildcard;
#endif
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
  bool session_zero_connection = false;
  bool media_zero_connection = false;
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
          media->video_offered = true;
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
            media->video.payload_type =
                candidate_order[0];
          }
          media_ip = session_ip;
          media_zero_connection = session_zero_connection;
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
      if (inet_aton(line.substr(9).c_str(), &address) != 0) {
        session_zero_connection = address.s_addr == 0;
        if (!session_zero_connection) {
          session_ip = ntohl(address.s_addr);
          media_ip = session_ip;
          media_rtcp_ip = session_ip;
        }
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
      if (inet_aton(line.substr(9).c_str(), &address) != 0) {
        media_zero_connection = address.s_addr == 0;
        if (!media_zero_connection) {
          media_ip = ntohl(address.s_addr);
          if (!media_rtcp_address_explicit) media_rtcp_ip = media_ip;
        }
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
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    } else if (in_video && line.rfind("a=imageattr:", 0) == 0) {
      const auto parsed = ImageAttributeParser::parse(std::string_view(line).substr(12),
          local_receive.width, local_receive.height, local_send.width, local_send.height);
      if (parsed.valid) {
        // RFC6236 permits send/recv on separate attribute lines. Merge each
        // direction independently; payload-specific entries override wildcard
        // entries only for the direction they actually declare.
        auto &target = parsed.explicit_payload ? imageattr_specific : imageattr_wildcard;
        target.valid = true;
        if (parsed.send_present) {
          target.send_compatible = (!target.send_present || target.send_compatible) && parsed.send_compatible;
          target.send_present = true;
        }
        if (parsed.recv_present) {
          target.recv_compatible = (!target.recv_present || target.recv_compatible) && parsed.recv_compatible;
          target.recv_present = true;
        }
      }
#endif
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

  if (!media->video_offered) return true;
  // A disabled answer is not an error and must not disturb audio.
  if (media_port == 0) return true;
  if (media_zero_connection) {
    // Legacy hold applies to this media section after connection inheritance.
    // Keep the prepared child's peer identity for a direction-only pause.
    if (!allow_zero_connection) return false;
    media->video_zero_connection = true;
    media_direction = 0;
    if (retained_ip != 0) media_ip = retained_ip;
    if (retained_rtcp_ip != 0) media_rtcp_ip = retained_rtcp_ip;
  }
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
    // Keep the peer's recommendation on our encoder direction. The decoder
    // retains its own budget in local_video_direction_capability_().
    negotiated.max_fps = send_compatible ? local_send.max_fps : local_receive.max_fps;
    if (send_compatible && receive_compatible) {
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
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    media->imageattr_state &= ~IMAGEATTR_PEER;
    if (imageattr_specific.valid || imageattr_wildcard.valid) {
      const auto &send_attr = imageattr_specific.send_present ? imageattr_specific : imageattr_wildcard;
      const auto &recv_attr = imageattr_specific.recv_present ? imageattr_specific : imageattr_wildcard;
      const bool compatible = (!local_receives || !send_attr.send_present || send_attr.send_compatible) &&
                              (!local_sends || !recv_attr.recv_present || recv_attr.recv_compatible);
      if (compatible) {
        media->imageattr_state |= IMAGEATTR_PEER;
      } else if (remote_is_answer && (media->imageattr_state & IMAGEATTR_OFFERED) &&
                 !(media->imageattr_state & IMAGEATTR_DISABLED)) {
        // RFC6236: do not silently accept an unusable answer. Preserve media
        // and let the existing SIP transaction owner offer once without it.
        media->imageattr_state |= IMAGEATTR_DISABLED | IMAGEATTR_FALLBACK;
      }
    }
#endif
    media->video = negotiated;
    media->video_ip = media_ip;
    media->video_port = media_port;
    media->video_rtcp_ip = media_rtcp_ip;
    media->video_rtcp_port = media_rtcp_port;
    media->video_send = local_sends;
    media->video_receive = local_receives;
    media->video_negotiated = true;
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
