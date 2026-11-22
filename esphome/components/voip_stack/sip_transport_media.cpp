#include "sip_transport_internal.h"

#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_SIP_TRANSPORT)

namespace esphome {
namespace voip_stack {

void SipTransport::set_media_config_(const AudioFormat &tx, const AudioFormat &rx,
                                     uint8_t tx_payload_type, uint8_t rx_payload_type,
                                     const RtpAudioPayloads *rx_payloads) {
  this->commit_audio_config_(tx, rx, tx_payload_type, rx_payload_type, rx_payloads,
                              tx.is_valid(), rx.is_valid());
  this->audio_zero_connection_ = false;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  this->set_opus_tx_constraints_({});
#endif
}

void SipTransport::commit_audio_config_(const AudioFormat &tx, const AudioFormat &rx,
                                        uint8_t tx_payload_type, uint8_t rx_payload_type,
                                        const RtpAudioPayloads *rx_payloads, bool send, bool receive) {
  portENTER_CRITICAL(&this->media_config_lock_);
  this->selected_tx_format_ = tx;
  this->selected_rx_format_ = rx;
  this->audio_send_enabled_.store(send && tx.is_valid(), std::memory_order_release);
  this->audio_receive_enabled_.store(receive && rx.is_valid(), std::memory_order_release);
  this->rtp_tx_payload_type_ = tx_payload_type;
  this->rtp_rx_payload_type_ = rx_payload_type;
  this->negotiated_rx_payloads_ = rx_payloads != nullptr ? *rx_payloads : RtpAudioPayloads{};
  if (rx_payloads == nullptr && rx.is_valid()) this->negotiated_rx_payloads_.add(rx_payload_type, rx);
  portEXIT_CRITICAL(&this->media_config_lock_);
}

SipTransport::NegotiatedMedia SipTransport::negotiated_media_() const {
  NegotiatedMedia media;
  this->get_media_config_(&media.tx, &media.rx, &media.tx_pt, &media.rx_pt, &media.rx_payloads);
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  media.opus_tx = this->get_opus_tx_constraints_();
#endif
  media.send = this->audio_send_enabled_.load(std::memory_order_acquire);
  media.receive = this->audio_receive_enabled_.load(std::memory_order_acquire);
  media.zero_connection = this->audio_zero_connection_;
  media.audio_ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  media.audio_port = this->remote_rtp_port_.load(std::memory_order_acquire);
  media.directional_audio = this->remote_directional_audio_v1_;
  media.shape = this->remote_media_shape_;
#ifdef USE_ESPHOME_VOIP_DTMF
  this->get_dtmf_config_(&media.dtmf.tx, &media.dtmf.rx);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  media.video_offered = this->video_offered_;
  media.video_negotiated = this->video_negotiated_;
  media.video_send = this->video_send_enabled_;
  media.video_receive = this->video_receive_enabled_;
  media.video_zero_connection = this->video_zero_connection_;
  media.video_ip = this->remote_video_ip_v4_;
  media.video_port = this->remote_video_rtp_port_;
  media.video_rtcp_ip = this->remote_video_rtcp_ip_v4_;
  media.video_rtcp_port = this->remote_video_rtcp_port_;
  media.video = this->negotiated_video_capability_;
  media.video_payloads = this->video_payloads_;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  media.imageattr_state = this->video_imageattr_state_;
#endif
#endif
  return media;
}

void SipTransport::commit_negotiated_media_(const NegotiatedMedia &media) {
  this->commit_audio_config_(media.tx, media.rx, media.tx_pt, media.rx_pt, &media.rx_payloads,
                              media.send, media.receive);
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  this->set_opus_tx_constraints_(media.opus_tx);
#endif
  this->audio_zero_connection_ = media.zero_connection;
  this->remote_rtp_ip_v4_.store(media.audio_ip, std::memory_order_release);
  this->remote_rtp_port_.store(media.audio_port, std::memory_order_release);
  this->remote_directional_audio_v1_ = media.directional_audio;
  this->remote_media_shape_ = media.shape;
#ifdef USE_ESPHOME_VOIP_DTMF
  this->set_dtmf_config_(media.dtmf.tx, media.dtmf.rx);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  this->video_offered_ = media.video_offered;
  this->video_negotiated_ = media.video_negotiated;
  this->video_send_enabled_ = media.video_send;
  this->video_receive_enabled_ = media.video_receive;
  this->video_zero_connection_ = media.video_zero_connection;
  this->remote_video_ip_v4_ = media.video_ip;
  this->remote_video_rtp_port_ = media.video_port;
  this->remote_video_rtcp_ip_v4_ = media.video_rtcp_ip;
  this->remote_video_rtcp_port_ = media.video_rtcp_port;
  this->negotiated_video_capability_ = media.video;
  this->video_payloads_ = media.video_payloads;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  this->video_imageattr_state_ = media.imageattr_state;
#endif
#endif
}

void SipTransport::exchange_negotiated_media_(NegotiatedMedia &media) {
  AudioFormat tx, rx;
  uint8_t tx_pt, rx_pt;
  RtpAudioPayloads payloads;
  this->get_media_config_(&tx, &rx, &tx_pt, &rx_pt, &payloads);
  const bool send = this->audio_send_enabled_.load(std::memory_order_acquire);
  const bool receive = this->audio_receive_enabled_.load(std::memory_order_acquire);
  this->commit_audio_config_(media.tx, media.rx, media.tx_pt, media.rx_pt, &media.rx_payloads,
                              media.send, media.receive);
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  const auto opus = this->get_opus_tx_constraints_();
  this->set_opus_tx_constraints_(media.opus_tx);
  media.opus_tx = opus;
#endif
  media.tx = tx; media.rx = rx; media.tx_pt = tx_pt; media.rx_pt = rx_pt;
  media.rx_payloads = payloads; media.send = send; media.receive = receive;
  media.audio_ip = this->remote_rtp_ip_v4_.exchange(media.audio_ip);
  media.audio_port = this->remote_rtp_port_.exchange(media.audio_port);
  using std::swap;
  swap(this->audio_zero_connection_, media.zero_connection);
  swap(this->remote_directional_audio_v1_, media.directional_audio);
  swap(this->remote_media_shape_, media.shape);
#ifdef USE_ESPHOME_VOIP_DTMF
  DtmfConfig dtmf;
  this->get_dtmf_config_(&dtmf.tx, &dtmf.rx);
  this->set_dtmf_config_(media.dtmf.tx, media.dtmf.rx);
  media.dtmf = dtmf;
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  swap(this->video_offered_, media.video_offered);
  swap(this->video_negotiated_, media.video_negotiated);
  swap(this->video_send_enabled_, media.video_send);
  swap(this->video_receive_enabled_, media.video_receive);
  swap(this->video_zero_connection_, media.video_zero_connection);
  swap(this->remote_video_ip_v4_, media.video_ip);
  swap(this->remote_video_rtp_port_, media.video_port);
  swap(this->remote_video_rtcp_ip_v4_, media.video_rtcp_ip);
  swap(this->remote_video_rtcp_port_, media.video_rtcp_port);
  swap(this->negotiated_video_capability_, media.video);
  swap(this->video_payloads_, media.video_payloads);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  swap(this->video_imageattr_state_, media.imageattr_state);
#endif
#endif
}

void SipTransport::get_media_config_(AudioFormat *tx, AudioFormat *rx,
                                     uint8_t *tx_payload_type, uint8_t *rx_payload_type,
                                     RtpAudioPayloads *rx_payloads) const {
  portENTER_CRITICAL(&this->media_config_lock_);
  if (tx != nullptr) *tx = this->selected_tx_format_;
  if (rx != nullptr) *rx = this->selected_rx_format_;
  if (tx_payload_type != nullptr) *tx_payload_type = this->rtp_tx_payload_type_;
  if (rx_payload_type != nullptr) *rx_payload_type = this->rtp_rx_payload_type_;
  if (rx_payloads != nullptr) *rx_payloads = this->negotiated_rx_payloads_;
  portEXIT_CRITICAL(&this->media_config_lock_);
}

#ifdef USE_ESPHOME_VOIP_STACK_OPUS
OpusReceiveConstraints SipTransport::get_opus_tx_constraints_() const {
  portENTER_CRITICAL(&this->media_config_lock_);
  const auto result = this->opus_tx_constraints_;
  portEXIT_CRITICAL(&this->media_config_lock_);
  return result;
}

void SipTransport::set_opus_tx_constraints_(const OpusReceiveConstraints &constraints) {
  portENTER_CRITICAL(&this->media_config_lock_);
  this->opus_tx_constraints_ = constraints;
  portEXIT_CRITICAL(&this->media_config_lock_);
}
#endif

bool SipTransport::received_audio_format_(uint8_t pt, AudioFormat *format,
                                          const uint8_t *payload, size_t payload_bytes) const {
  if (!this->audio_receive_enabled_.load(std::memory_order_acquire)) return false;
  AudioFormat mapping;
  portENTER_CRITICAL(&this->media_config_lock_);
  const bool found = this->negotiated_rx_payloads_.find(pt, &mapping);
  portEXIT_CRITICAL(&this->media_config_lock_);
  if (!found) return false;
  if (payload == nullptr) {
    if (format != nullptr) *format = mapping;
    return true;
  }
  if (payload_bytes == 0 || payload_bytes > this->udp_max_payload_) return false;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  const int opus_samples = mapping.codec == AudioCodec::OPUS
      ? opus_packet_get_nb_samples(payload, static_cast<opus_int32>(payload_bytes), 48000) : 0;
#endif
  // The PT fixes encoding/clock/channels, not packet duration. Configured
  // variants are immutable while RTP runs, so this bounded scan needs no lock.
  for (uint8_t i = 0; i < this->offer_rx_formats_.count; ++i) {
    const auto &candidate = this->offer_rx_formats_.formats[i];
    if (!candidate.is_valid() || !audio_formats_share_rtp_mapping(candidate, mapping) ||
        audio_format_rtp_encoding(candidate, this->udp_max_payload_) == nullptr) continue;
    bool matches = false;
    if (candidate.codec == AudioCodec::PCM || candidate.codec == AudioCodec::G722)
      matches = candidate.nominal_rtp_payload_bytes() == payload_bytes;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
    else if (candidate.codec == AudioCodec::OPUS)
      matches = opus_samples > 0 && static_cast<uint32_t>(opus_samples) == 48U * candidate.frame_ms;
#endif
    if (matches) {
      if (format != nullptr) *format = candidate;
      return true;
    }
  }
  return false;
}

void SipTransport::set_audio_formats(const AudioFormatList &tx, const AudioFormatList &rx) {
  this->offer_tx_formats_ = tx;
  this->offer_rx_formats_ = rx;
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  const AudioFormat *opus_tx = nullptr;
  const AudioFormat *opus_rx = nullptr;
  for (uint8_t i = 0; i < this->offer_tx_formats_.count && opus_tx == nullptr; i++)
    if (this->offer_tx_formats_.formats[i].codec == AudioCodec::OPUS)
      opus_tx = &this->offer_tx_formats_.formats[i];
  for (uint8_t i = 0; i < this->offer_rx_formats_.count && opus_rx == nullptr; i++)
    if (this->offer_rx_formats_.formats[i].codec == AudioCodec::OPUS)
      opus_rx = &this->offer_rx_formats_.formats[i];
  this->opus_codec_ready_ = this->opus_codec_.configure(
      opus_tx, opus_rx, this->udp_max_payload_);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_G722
  const AudioFormat *g722_tx = nullptr;
  const AudioFormat *g722_rx = nullptr;
  for (uint8_t i = 0; i < tx.count && g722_tx == nullptr; ++i)
    if (tx.formats[i].codec == AudioCodec::G722) g722_tx = &tx.formats[i];
  for (uint8_t i = 0; i < rx.count && g722_rx == nullptr; ++i)
    if (rx.formats[i].codec == AudioCodec::G722) g722_rx = &rx.formats[i];
  this->g722_codec_ready_ = this->g722_codec_.configure(g722_tx, g722_rx, this->udp_max_payload_);
#endif
  ESP_LOGI(TAG, "SIP media capabilities: tx=%u rx=%u",
           (unsigned) this->offer_tx_formats_.count,
           (unsigned) this->offer_rx_formats_.count);
}

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
void SipTransport::set_video_endpoints(EncodedVideoSource *source,
                                       EncodedVideoSink *sink) {
  this->video_source_ = source;
  this->video_sink_ = sink;
  if (this->video_session_ != nullptr) {
    this->video_session_->set_source(source);
    this->video_session_->set_sink(sink);
    if (!this->video_session_->prepare_buffers())
      ESP_LOGE(TAG, "Video endpoint buffers unavailable at setup");
  }
}

void SipTransport::set_video_config(VideoCodec codec, uint16_t rtp_port,
                                    uint8_t offer_payload_type,
                                    size_t max_rtp_payload) {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  if (codec != VideoCodec::JPEG || offer_payload_type != 26) {
    ESP_LOGE(TAG, "JPEG video build requires codec JPEG and static PT 26");
  }
#else
  if (codec != VideoCodec::H264 || offer_payload_type < 96) {
    ESP_LOGE(TAG, "H.264 video build requires codec H264 and a dynamic PT");
  }
#endif
  this->video_codec_ = codec;
  this->video_rtp_port_ = rtp_port;
  this->video_offer_payload_type_ = offer_payload_type;
  this->video_max_rtp_payload_ = max_rtp_payload;
  if (this->video_session_ == nullptr) {
    this->video_session_ =
        std::make_unique<VideoRtpSession>(this->task_stacks_in_psram_);
  }
  this->video_session_->set_source(this->video_source_);
  this->video_session_->set_sink(this->video_sink_);
  this->video_session_->set_local_port(this->video_rtp_port_);
  this->video_session_->set_max_payload(this->video_max_rtp_payload_);
  if (!this->video_session_->prepare_buffers())
    ESP_LOGE(TAG, "Video endpoint buffers unavailable at setup");
}

VideoCapability SipTransport::local_video_send_capability_() const {
  VideoCapability capability;
  if (this->video_source_ == nullptr) {
    capability.max_fps = 0;
    return capability;
  }
  capability = this->video_source_->get_video_capability();
  if ((this->video_codec_ == VideoCodec::JPEG && !capability.is_jpeg()) ||
      (this->video_codec_ == VideoCodec::H264 && !capability.is_h264())) {
    ESP_LOGE(TAG, "Configured video codec does not match the source capability");
    return {};
  }
  capability.payload_type =
      capability.is_jpeg() ? 26 : this->video_offer_payload_type_;
  capability.packetization_mode = capability.is_h264() ? 1 : 0;
  capability.rtcp_feedback_pli = capability.is_h264();
  capability.rtcp_feedback_fir = capability.is_h264();
  capability.clock_rate = 90000;
  return capability;
}

VideoCapability SipTransport::local_video_receive_capability_() const {
  VideoCapability capability;
  if (this->video_sink_ == nullptr) {
    capability.max_fps = 0;
    return capability;
  }
  capability = this->video_sink_->get_receive_video_capability();
  if ((this->video_codec_ == VideoCodec::JPEG && !capability.is_jpeg()) ||
      (this->video_codec_ == VideoCodec::H264 && !capability.is_h264())) {
    ESP_LOGE(TAG, "Configured video codec does not match the sink capability");
    return {};
  }
  capability.payload_type =
      capability.is_jpeg() ? 26 : this->video_offer_payload_type_;
  capability.packetization_mode = capability.is_h264() ? 1 : 0;
  capability.rtcp_feedback_pli = capability.is_h264();
  capability.rtcp_feedback_fir = capability.is_h264();
  capability.clock_rate = 90000;
  return capability;
}

VideoCapability SipTransport::local_video_direction_capability_(
    const VideoCapability &negotiated, bool send) const {
  VideoCapability capability =
      send ? this->local_video_send_capability_()
           : this->local_video_receive_capability_();
  if (!capability.valid() ||
      capability.encoding != negotiated.encoding ||
      !negotiated.valid()) {
    return {};
  }
  // PT, clock and codec parameters are the shared RTP contract. Dimensions
  // stay directional: RFC 2435 carries each JPEG frame's actual width/height
  // in-band, and an encoder need not have the same envelope as the decoder.
  capability.payload_type = send ? negotiated.payload_type : negotiated.receive_payload_type();
  capability.rx_payload_type = negotiated.rx_payload_type;
  capability.clock_rate = negotiated.clock_rate;
  capability.packetization_mode = negotiated.packetization_mode;
  capability.level_asymmetry_allowed =
      negotiated.level_asymmetry_allowed;
  capability.rtcp_feedback_pli =
      capability.rtcp_feedback_pli && negotiated.rtcp_feedback_pli;
  capability.rtcp_feedback_fir =
      capability.rtcp_feedback_fir && negotiated.rtcp_feedback_fir;
  if (negotiated.max_bitrate_bps != 0 &&
      (capability.max_bitrate_bps == 0 ||
       negotiated.max_bitrate_bps < capability.max_bitrate_bps)) {
    capability.max_bitrate_bps = negotiated.max_bitrate_bps;
  }
  if (send)
    capability.max_fps = std::min(capability.max_fps, negotiated.max_fps);
  return capability;
}

bool SipTransport::prepare_video_session_locked_() {
  return this->prepare_video_session_locked_(this->negotiated_media_());
}

bool SipTransport::prepare_video_session_locked_(const NegotiatedMedia &media) {
  if (!media.video_negotiated) return true;
  VOIP_STARTUP_EVENT(VIDEO_PREPARE);
  if (this->video_session_ == nullptr) return false;
  if (this->video_session_->is_running()) {
    return this->video_session_->negotiation_matches(
               media.video,
               this->local_video_direction_capability_(
                   media.video, true),
               this->local_video_direction_capability_(
                   media.video, false),
               media.video_ip, media.video_port,
               media.video_rtcp_ip,
               media.video_rtcp_port) &&
           this->video_session_->can_request_media_direction(
               media.video_send, media.video_receive);
  }
  const bool prepared = this->video_session_->set_negotiated(
             media.video,
             this->local_video_direction_capability_(
                 media.video, true),
             this->local_video_direction_capability_(
                 media.video, false),
             media.video_ip, media.video_port,
             media.video_rtcp_ip,
             media.video_rtcp_port, media.video_send,
             media.video_receive) &&
         this->video_session_->start(false);
  VOIP_STARTUP_EVENT(VIDEO_PREPARED);
  return prepared;
}

void SipTransport::reset_video_negotiation_() {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  this->video_imageattr_state_ &= ~IMAGEATTR_PEER;
#endif
  this->video_offered_ = false;
  this->video_negotiated_ = false;
  this->video_send_enabled_ = false;
  this->video_receive_enabled_ = false;
  this->video_zero_connection_ = false;
  this->remote_video_ip_v4_ = 0;
  this->remote_video_rtp_port_ = 0;
  this->remote_video_rtcp_ip_v4_ = 0;
  this->remote_video_rtcp_port_ = 0;
  this->negotiated_video_capability_ = {};
}
#endif

bool SipTransport::start_audio_path() {
  LockGuard media_lock(this->media_lifecycle_mutex_);
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Transport is stopping; media start rejected");
    return false;
  }
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::CLEANING ||
      phase == MediaLifecyclePhase::SHUTTING_DOWN) {
    ESP_LOGW(TAG, "Media lifecycle is not ready for a new session");
    return false;
  }
  return this->prepare_media_path_locked_() &&
         this->commit_media_path_locked_();
}

bool SipTransport::prepare_media_path() {
  LockGuard media_lock(this->media_lifecycle_mutex_);
  if (!this->running_.load(std::memory_order_acquire) ||
      this->transport_stopping_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Transport is stopping; media prepare rejected");
    return false;
  }
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::PREPARED ||
      (phase == MediaLifecyclePhase::ACTIVE &&
       this->rtp_running_.load(std::memory_order_acquire))) {
    return true;
  }
  if (phase != MediaLifecyclePhase::IDLE) {
    ESP_LOGW(TAG, "Media lifecycle is not ready for preparation");
    return false;
  }
  return this->prepare_media_path_locked_();
}

bool SipTransport::prepare_media_path_locked_() {
  VOIP_STARTUP_EVENT(PREPARE);
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::PREPARED ||
      (phase == MediaLifecyclePhase::ACTIVE &&
       this->rtp_running_.load(std::memory_order_acquire))) {
    return true;
  }
  if (phase != MediaLifecyclePhase::IDLE) {
    ESP_LOGE(TAG, "Media lifecycle is not idle");
    return false;
  }
  if (this->rtp_task_handle_ == nullptr ||
      this->rtp_task_terminate_.load(std::memory_order_acquire) ||
      !this->rtp_task_quiesced_.load(std::memory_order_acquire)) {
    ESP_LOGE(TAG, "RTP task is unavailable for a new media session");
    return false;
  }
  this->reset_rtp_latch_();
  {
    LockGuard clock_lock(this->rtp_socket_mutex_);
    this->rtp_sequence_.store(static_cast<uint16_t>(esp_random()), std::memory_order_release);
    this->rtp_timestamp_.store(esp_random(), std::memory_order_release);
    this->rtp_ssrc_ = esp_random();
    AudioFormat tx;
    this->get_media_config_(&tx, nullptr, nullptr, nullptr);
    this->audio_tx_clock_rate_ = audio_format_rtp_clock_rate(tx);
    ++this->audio_tx_clock_generation_;
  }
#ifdef USE_ESPHOME_VOIP_DTMF
  if (this->dtmf_queue_ != nullptr) xQueueReset(this->dtmf_queue_);
  this->dtmf_generation_.fetch_add(1, std::memory_order_release);
  this->rtp_tx_clock_timestamp_ = this->rtp_timestamp_.load(std::memory_order_relaxed);
  this->rtp_tx_clock_ms_ = millis();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  AudioFormat sink_format;
  RtpAudioPayloads rx_payloads;
  this->get_media_config_(nullptr, &sink_format, nullptr, nullptr, &rx_payloads);
  if (sink_format.is_valid() &&
      !this->pcm_receive_converter_.prepare(
          configured_decoded_receive_formats(rx_payloads, this->offer_rx_formats_, this->udp_max_payload_),
          decoded_pcm_format(pcm_playout_format(sink_format, this->offer_rx_formats_)))) {
    ESP_LOGE(TAG, "Unable to prepare negotiated PCM receive formats");
    return false;
  }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  AudioFormat prepared_tx;
  AudioFormat prepared_rx;
  RtpAudioPayloads prepared_payloads;
  this->get_media_config_(&prepared_tx, &prepared_rx, nullptr, nullptr, &prepared_payloads);
  // The preferred playout codec can be PCM while an advertised Opus fallback
  // uses the same PCM sink. Validate every decoder that can receive packets.
  for (uint8_t i = 0; i < prepared_payloads.count; ++i) {
    if (prepared_payloads.entries[i].format.codec == AudioCodec::OPUS) {
      AudioFormat packet_sink = prepared_rx;
      packet_sink.frame_ms = prepared_payloads.entries[i].format.frame_ms;
      if (!same_decoded_audio_geometry(prepared_payloads.entries[i].format, packet_sink)) return false;
      prepared_rx = prepared_payloads.entries[i].format;
      break;
    }
  }
  if (!this->opus_codec_.ready_for(prepared_tx, prepared_rx)) {
    ESP_LOGE(TAG, "Negotiated Opus format was not preallocated at setup");
    return false;
  }
  this->opus_codec_.reset();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_G722
  AudioFormat g722_tx, g722_rx;
  RtpAudioPayloads g722_payloads;
  this->get_media_config_(&g722_tx, &g722_rx, nullptr, nullptr, &g722_payloads);
  for (uint8_t i = 0; i < g722_payloads.count; ++i)
    if (g722_payloads.entries[i].format.codec == AudioCodec::G722) {
      g722_rx = g722_payloads.entries[i].format;
      break;
    }
  if (!this->g722_codec_.ready_for(g722_tx, g722_rx)) {
    ESP_LOGE(TAG, "Negotiated G722 format was not preallocated at setup");
    return false;
  }
  this->g722_codec_.reset();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  this->audio_tx_slow_send_calls_ = 0;
  this->audio_tx_send_failures_ = 0;
  this->audio_tx_max_send_us_ = 0;
  this->audio_tx_last_debug_log_ms_ = millis();
#endif
  if (this->rtp_socket_ < 0 &&
      !this->bind_udp_(&this->rtp_socket_, this->rtp_port_, "RTP")) return false;
  xSemaphoreTake(this->rtp_cleanup_done_, 0);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->video_negotiated_) {
    if (!this->prepare_video_session_locked_()) {
      // Video is subordinate media. A late PSRAM/codec failure must not reject
      // an otherwise healthy SIP audio answer.
      ESP_LOGE(TAG,
               "Negotiated video failed to prepare; answering audio-only");
      if (this->video_session_ != nullptr)
        this->video_session_->request_stop();
      this->video_negotiated_ = false;
    }
  }
#endif
  // PREPARED owns the bound sockets and dormant codec resources, but the RTP
  // worker, camera source and renderer direction remain inactive until the
  // final SIP answer has been sent successfully.
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::PREPARED,
                                      std::memory_order_release);
  VOIP_STARTUP_EVENT(PREPARED);
  return true;
}

bool SipTransport::commit_media_path() {
  bool committed = false;
  {
    LockGuard media_lock(this->media_lifecycle_mutex_);
    if (!this->running_.load(std::memory_order_acquire) ||
        this->transport_stopping_.load(std::memory_order_acquire)) {
      ESP_LOGW(TAG, "Transport is stopping; media commit rejected");
    } else {
      committed = this->commit_media_path_locked_();
    }
  }
  if (!committed)
    this->terminate_dialog_after_media_commit_failure_();
  return committed;
}

bool SipTransport::commit_media_path_locked_() {
  VOIP_STARTUP_EVENT(COMMIT);
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::ACTIVE &&
      this->rtp_running_.load(std::memory_order_acquire)) {
    return true;
  }
  if (phase != MediaLifecyclePhase::PREPARED) {
    ESP_LOGE(TAG, "Media lifecycle is not prepared");
    return false;
  }
  if (this->rtp_task_handle_ == nullptr ||
      this->rtp_task_terminate_.load(std::memory_order_acquire) ||
      !this->rtp_task_quiesced_.load(std::memory_order_acquire) ||
      this->rtp_socket_ < 0) {
    ESP_LOGE(TAG, "Prepared RTP resources are unavailable");
    return false;
  }

  this->rtp_task_quiesced_.store(false, std::memory_order_release);
  this->rtp_running_.store(true, std::memory_order_release);
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::ACTIVE,
                                      std::memory_order_release);
  VOIP_STARTUP_EVENT(NOTIFY);
  xTaskNotifyGive(this->rtp_task_handle_);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  VOIP_STARTUP_EVENT(VIDEO_BEGIN);
  if (this->video_negotiated_ &&
      !this->video_session_->request_media_direction(
          this->video_send_enabled_, this->video_receive_enabled_)) {
    // Video remains subordinate after the SIP transaction commits.
    ESP_LOGE(TAG, "Negotiated video failed to activate; keeping audio active");
    this->video_session_->request_stop();
    this->video_negotiated_ = false;
  }
  this->emit_video_active_state_(
      this->video_negotiated_ && this->video_session_ != nullptr &&
      this->video_session_->is_running() &&
      (this->video_send_enabled_ || this->video_receive_enabled_));
  this->emit_video_send_state_(
      this->video_negotiated_ && this->video_send_enabled_, false);
  VOIP_STARTUP_EVENT(VIDEO_END);
#endif
  VOIP_STARTUP_EVENT(COMMITTED);
  return true;
}

void SipTransport::terminate_dialog_after_media_commit_failure_() {
  // commit_media_path() is the UAS post-200 path. Observe the server
  // transaction under its owning mutex only after releasing the media mutex,
  // preserving the established dialog->media lock order.
  LockGuard dialog_lock(this->dialog_mutex_);
  const bool accepted_dialog =
      !this->call_id_.empty() && !this->completed_invite_.empty() &&
      this->completed_invite_.status >= 200 &&
      this->completed_invite_.status < 300 &&
      this->completed_invite_.call_id == this->call_id_;
  if (!accepted_dialog) return;

  if (this->defer_bye_until_uas_ack_locked_()) return;

  // The ACK may have raced the main-loop commit. In that case the dialog is
  // already confirmed and can be terminated immediately with the normal BYE
  // transaction rather than being reset locally.
  if (!this->send_bye_unlocked_(this->call_id_)) {
    ESP_LOGE(TAG,
             "Failed to send BYE after post-answer media commit failure");
  }
}

void SipTransport::abort_media_path() {
  this->stop_audio_path();
}

void SipTransport::stop_audio_path() {
  LockGuard media_lock(this->media_lifecycle_mutex_);
  this->request_audio_path_stop_locked_();
}

void SipTransport::request_audio_path_stop_locked_() {
  this->close_media_session_();
  const MediaLifecyclePhase phase =
      this->media_lifecycle_phase_.load(std::memory_order_acquire);
  if (phase == MediaLifecyclePhase::CLEANING ||
      phase == MediaLifecyclePhase::SHUTTING_DOWN) {
    return;
  }
  const bool audio_was_running =
      this->rtp_running_.exchange(false, std::memory_order_acq_rel);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  const bool video_was_running =
      this->video_session_ != nullptr &&
      this->video_session_->is_running();
  if (video_was_running) this->emit_video_active_state_(false);
#else
  constexpr bool video_was_running = false;
#endif
  if (phase == MediaLifecyclePhase::IDLE && !audio_was_running &&
      !video_was_running && this->rtp_socket_ < 0) {
    return;
  }
  this->media_lifecycle_phase_.store(MediaLifecyclePhase::CLEANING,
                                      std::memory_order_release);
  if (this->rtp_task_handle_ == nullptr) {
    // setup()/destructor failure path only: no worker can own these resources.
    this->finish_audio_path_stop_();
    this->rtp_task_quiesced_.store(true, std::memory_order_release);
    this->media_lifecycle_phase_.store(MediaLifecyclePhase::IDLE,
                                        std::memory_order_release);
    return;
  }
  if (this->rtp_cleanup_done_ != nullptr)
    xSemaphoreTake(this->rtp_cleanup_done_, 0);
  this->rtp_task_quiesced_.store(false, std::memory_order_release);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  // Atomic gate+wake only. RTCP, source/sink callbacks and joins belong to the
  // RTP lifecycle worker, never to a SIP or ESPHome-loop caller.
  if (this->video_session_ != nullptr)
    this->video_session_->request_stop();
#endif
  if (audio_was_running)
    this->wake_rtp_task_();
  else
    xTaskNotifyGive(this->rtp_task_handle_);
}

void SipTransport::finish_audio_path_stop_() {
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
  if (this->video_session_ != nullptr) this->video_session_->stop();
#endif
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (this->rtp_socket_ >= 0) {
    close(this->rtp_socket_);
    this->rtp_socket_ = -1;
  }
}

TransportAudioTxContext SipTransport::audio_tx_frame_context(const AudioFormat &format) {
  TransportAudioTxContext context;
  context.format = format;
  if (!format.is_valid()) return context;
  LockGuard clock_lock(this->rtp_socket_mutex_);
  if (!this->rtp_running_.load(std::memory_order_acquire)) return context;
  const uint32_t epoch = this->media_proposal_epoch_.load(std::memory_order_acquire);
  if (!(epoch & 1U)) {
    AudioFormat committed;
    this->get_media_config_(&committed, nullptr, nullptr, nullptr);
    if (epoch == this->media_proposal_epoch_.load(std::memory_order_acquire)) {
      const uint32_t rate = audio_format_rtp_clock_rate(committed);
      if (rate != this->audio_tx_clock_rate_) {
        this->audio_tx_clock_rate_ = rate;
        ++this->audio_tx_clock_generation_;
      }
      if (!(format == committed)) return context;
    }
  }
  context.clock_rate = audio_format_rtp_clock_rate(format);
  if (context.clock_rate == 0 || context.clock_rate != this->audio_tx_clock_rate_) return context;
  context.generation = this->audio_tx_clock_generation_;
  context.ssrc = this->rtp_ssrc_;
  context.valid = true;
  return context;
}

void SipTransport::send_audio_frame(const uint8_t *pcm, size_t bytes) {
  // Compatibility callers have not captured an active producer contract.
  if (this->media_proposal_epoch_.load(std::memory_order_acquire) & 1U) return;
  AudioFormat format;
  this->get_media_config_(&format, nullptr, nullptr, nullptr);
  this->send_audio_frame(pcm, bytes, this->audio_tx_frame_context(format));
}

void SipTransport::send_audio_frame(const uint8_t *pcm, size_t bytes, const TransportAudioTxContext &context) {
  if (!context.valid || pcm == nullptr || bytes == 0 || !context.format.is_valid() ||
      bytes != context.format.nominal_frame_bytes()) return;
  const AudioFormat tx_format = context.format;
  const uint32_t pcm_samples = bytes / tx_format.container_bytes_per_sample() / tx_format.channels;
  const uint64_t ticks = static_cast<uint64_t>(context.clock_rate) * pcm_samples;
  if (context.clock_rate == 0 || ticks % tx_format.sample_rate != 0) return;
  const uint32_t timestamp_samples = static_cast<uint32_t>(ticks / tx_format.sample_rate);
  if (timestamp_samples == 0) return;
  const uint32_t proposal_epoch =
      this->media_proposal_epoch_.load(std::memory_order_acquire);
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  const auto opus_constraints = this->get_opus_tx_constraints_();
#endif
  bool stable = (proposal_epoch & 1U) == 0;
  AudioFormat committed;
  uint8_t tx_payload_type = 96;
  if (stable) {
    this->get_media_config_(&committed, nullptr, &tx_payload_type, nullptr);
    stable = proposal_epoch == this->media_proposal_epoch_.load(std::memory_order_acquire);
  }
  uint32_t ts;
  {
    LockGuard clock_lock(this->rtp_socket_mutex_);
    stable = stable && proposal_epoch == this->media_proposal_epoch_.load(std::memory_order_acquire);
    if (stable) {
      const uint32_t rate = audio_format_rtp_clock_rate(committed);
      if (rate != this->audio_tx_clock_rate_) {
        this->audio_tx_clock_rate_ = rate;
        ++this->audio_tx_clock_generation_;
      }
    }
    if (!this->rtp_running_.load(std::memory_order_acquire) || context.ssrc != this->rtp_ssrc_ ||
        context.generation != this->audio_tx_clock_generation_ || context.clock_rate != this->audio_tx_clock_rate_) return;
    // Sampling time belongs to this consumed frame, independently of whether
    // a proposal, hold or local encoder failure suppresses its RTP packet.
    ts = this->rtp_timestamp_.fetch_add(timestamp_samples, std::memory_order_acq_rel);
    if (!stable || !(committed == tx_format) || !this->audio_send_enabled_.load(std::memory_order_acquire)) return;
  }
  // PCM conversion is the expensive part. Keep it outside the socket mutex;
  // teardown flips rtp_running_ first and the short final critical section
  // below prevents close-vs-send without delaying the media loop.
  uint8_t packet[1500];
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  if (tx_format.codec == AudioCodec::OPUS) {
    bytes = this->opus_codec_.encode(pcm, bytes, tx_format, packet + 12,
                                     sizeof(packet) - 12, opus_constraints);
  } else
#endif
#ifdef USE_ESPHOME_VOIP_STACK_G722
  if (tx_format.codec == AudioCodec::G722) {
    bytes = this->g722_codec_.encode(pcm, bytes, tx_format, packet + 12, sizeof(packet) - 12);
  } else
#endif
  {
#ifdef USE_ESPHOME_VOIP_STACK_PCM
    bytes = pcm_to_rtp_payload(pcm, bytes, tx_format, packet + 12,
                               sizeof(packet) - 12);
#else
    bytes = 0;
#endif
  }
  if (bytes == 0 || bytes > this->udp_max_payload_) {
    // Sequence numbers count packets, while timestamps follow the sampling
    // clock. A locally discarded PCM frame therefore advances only time.
    return;
  }
  if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
      proposal_epoch) {
    return;
  }
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (!this->rtp_running_.load(std::memory_order_acquire) || this->rtp_socket_ < 0 ||
      !this->audio_send_enabled_.load(std::memory_order_acquire) || context.ssrc != this->rtp_ssrc_ ||
      context.generation != this->audio_tx_clock_generation_ || context.clock_rate != this->audio_tx_clock_rate_) return;
  const uint32_t ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_rtp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return;
  if (this->media_proposal_epoch_.load(std::memory_order_acquire) !=
      proposal_epoch) {
    return;
  }
  packet[0] = 0x80;
  packet[1] = tx_payload_type & 0x7F;
  const uint16_t seq = this->rtp_sequence_.fetch_add(1, std::memory_order_acq_rel);
  packet[2] = static_cast<uint8_t>(seq >> 8);
  packet[3] = static_cast<uint8_t>(seq & 0xFF);
  packet[4] = static_cast<uint8_t>(ts >> 24);
  packet[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
  packet[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
  packet[7] = static_cast<uint8_t>(ts & 0xFF);
#ifdef USE_ESPHOME_VOIP_DTMF
  this->rtp_tx_clock_ms_ = millis();
  this->rtp_tx_clock_timestamp_ = ts;
#endif
  packet[8] = static_cast<uint8_t>(this->rtp_ssrc_ >> 24);
  packet[9] = static_cast<uint8_t>((this->rtp_ssrc_ >> 16) & 0xFF);
  packet[10] = static_cast<uint8_t>((this->rtp_ssrc_ >> 8) & 0xFF);
  packet[11] = static_cast<uint8_t>(this->rtp_ssrc_ & 0xFF);
  struct sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(ip);
  dest.sin_port = htons(port);
  if (!this->rtp_running_.load(std::memory_order_acquire)) return;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t send_started_us = micros();
#endif
  const int sent = sendto(this->rtp_socket_, packet, 12 + bytes, 0,
                          reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_DEBUG
  const uint32_t send_elapsed_us = micros() - send_started_us;
  this->audio_tx_max_send_us_ =
      std::max(this->audio_tx_max_send_us_, send_elapsed_us);
  if (send_elapsed_us >= 5000U) this->audio_tx_slow_send_calls_++;
  if (sent <= 0) this->audio_tx_send_failures_++;
  const uint32_t now = millis();
  if (now - this->audio_tx_last_debug_log_ms_ >= 5000U) {
    this->audio_tx_last_debug_log_ms_ = now;
    ESP_LOGI(TAG,
             "Audio RTP TX: packets=%u slow_send=%u send_fail=%u "
             "max_send_us=%u",
             (unsigned) this->rtp_tx_packets_.load(
                 std::memory_order_acquire),
             (unsigned) this->audio_tx_slow_send_calls_,
             (unsigned) this->audio_tx_send_failures_,
             (unsigned) this->audio_tx_max_send_us_);
  }
#endif
  if (sent > 0) {
    this->rtp_tx_packets_.fetch_add(1, std::memory_order_acq_rel);
    this->rtp_tx_bytes_.fetch_add(static_cast<uint32_t>(sent), std::memory_order_acq_rel);
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_HOSTED_AUDIO_PACING
    if (this->video_session_ != nullptr)
      this->video_session_->notify_audio_packet_sent();
#endif
  }
}

#if defined(USE_ESPHOME_VOIP_STACK_OPUS) || defined(USE_ESPHOME_VOIP_STACK_G722)
size_t SipTransport::decode_audio_payload(const uint8_t *payload,
                                          size_t payload_bytes,
                                          const AudioFormat &format,
                                          uint8_t *pcm,
                                          size_t pcm_capacity) {
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  if (format.codec == AudioCodec::OPUS)
    return this->opus_codec_.decode(payload, payload_bytes, format, pcm, pcm_capacity);
#endif
#ifdef USE_ESPHOME_VOIP_STACK_G722
  if (format.codec == AudioCodec::G722)
    return this->g722_codec_.decode(payload, payload_bytes, format, pcm, pcm_capacity);
#endif
  return 0;
}

#endif
void SipTransport::reset_audio_decoder() {
#ifdef USE_ESPHOME_VOIP_STACK_OPUS
  this->opus_codec_.reset_decoder();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_G722
  this->g722_codec_.reset_decoder();
#endif
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  this->pcm_receive_converter_.reset();
#endif
}

size_t SipTransport::convert_received_pcm(const AudioFormat &source, const uint8_t *pcm,
                                           size_t bytes, uint8_t *output, size_t capacity) {
#ifdef USE_ESPHOME_VOIP_STACK_SPEAKER
  return this->pcm_receive_converter_.convert(source, pcm, bytes, output, capacity);
#else
  return 0;
#endif
}

}  // namespace voip_stack
}  // namespace esphome

#endif
