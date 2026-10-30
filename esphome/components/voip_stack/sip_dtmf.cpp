#include "sip_transport.h"
#if defined(USE_ESP32) && defined(USE_ESPHOME_VOIP_DTMF)
#include <lwip/sockets.h>
#include "esphome/core/hal.h"

namespace esphome::voip_stack {
void SipTransport::get_dtmf_config_(DtmfFormat *tx, DtmfFormat *rx, uint32_t *epoch) const {
  portENTER_CRITICAL(&this->media_config_lock_);
  if (tx != nullptr) *tx = this->dtmf_tx_format_;
  if (rx != nullptr) *rx = this->dtmf_rx_format_;
  if (epoch != nullptr) *epoch = this->media_proposal_epoch_.load(std::memory_order_relaxed);
  portEXIT_CRITICAL(&this->media_config_lock_);
}
void SipTransport::set_dtmf_config_(const DtmfFormat &tx, const DtmfFormat &rx) {
  portENTER_CRITICAL(&this->media_config_lock_);
  this->dtmf_tx_format_ = tx; this->dtmf_rx_format_ = rx;
  this->media_proposal_epoch_.fetch_add(2, std::memory_order_release);
  portEXIT_CRITICAL(&this->media_config_lock_);
}
void SipTransport::append_dtmf_sdp_(std::string *payloads, std::string *maps, uint8_t clocks) {
  for (unsigned i = 0; i < 7; ++i) if (clocks & (1U << i)) {
    const auto pt = std::to_string(120 + i);
    *payloads += " " + pt;
    *maps += "a=rtpmap:" + pt + " telephone-event/" + std::to_string(DTMF_CLOCKS[i]) + "\r\n";
    *maps += "a=fmtp:" + pt + " 0-15\r\n";
  }
}
bool SipTransport::can_send_dtmf_locked_(const DtmfFormat &format, uint32_t epoch) const {
  return format.valid() && this->dtmf_queue_ != nullptr && this->rtp_socket_ >= 0 &&
      this->media_active_.load(std::memory_order_acquire) && this->rtp_running_.load(std::memory_order_acquire) &&
      (this->get_audio_direction_() & 1U) && !(this->media_proposal_epoch_.load(std::memory_order_acquire) & 1U) &&
      this->media_proposal_epoch_.load(std::memory_order_acquire) == epoch &&
      this->remote_rtp_ip_v4_.load(std::memory_order_acquire) != 0 && this->remote_rtp_port_.load(std::memory_order_acquire) != 0 &&
      uxQueueSpacesAvailable(this->dtmf_queue_) != 0;
}
bool SipTransport::can_send_dtmf() const {
  LockGuard dialog_lock(this->dialog_mutex_);
  DtmfFormat format; uint32_t epoch = 0; this->get_dtmf_config_(&format, nullptr, &epoch);
  LockGuard socket_lock(this->rtp_socket_mutex_);
  return this->can_send_dtmf_locked_(format, epoch);
}
bool SipTransport::send_dtmf(char digit, uint16_t duration_ms) {
  LockGuard dialog_lock(this->dialog_mutex_);
  const int event = dtmf_event(digit);
  DtmfFormat format; uint32_t epoch = 0; this->get_dtmf_config_(&format, nullptr, &epoch);
  if (event < 0 || !(format.events & (1U << event)) || duration_ms < 40 || duration_ms > 8000 ||
      uint64_t(duration_ms) * format.clock / 1000 > 65535) return false;
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (!this->can_send_dtmf_locked_(format, epoch)) return false;
  // Wake before publishing the command. The RTP worker takes the same short
  // socket lock before dequeueing, so it cannot consume an empty wakeup.
  sockaddr_in self{}; self.sin_family = AF_INET; self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  self.sin_port = htons(this->rtp_port_); constexpr uint8_t wake = 1;
  if (sendto(this->rtp_socket_, &wake, sizeof(wake), 0, reinterpret_cast<sockaddr *>(&self), sizeof(self)) != static_cast<int>(sizeof(wake))) return false;
  const DtmfRequest request{epoch, duration_ms, static_cast<uint8_t>(event)};
  return xQueueSend(this->dtmf_queue_, &request, 0) == pdTRUE;
}
bool SipTransport::send_dtmf_packet_(const DtmfSender::Report &report, const DtmfFormat &format,
                                    uint32_t epoch, uint32_t ssrc, uint16_t *separate_sequence) {
  LockGuard socket_lock(this->rtp_socket_mutex_);
  if (!this->rtp_running_.load(std::memory_order_acquire) || !this->media_active_.load(std::memory_order_acquire) ||
      this->rtp_socket_ < 0 || this->media_proposal_epoch_.load(std::memory_order_acquire) != epoch ||
      !(this->get_audio_direction_() & 1U) || (this->media_proposal_epoch_.load(std::memory_order_acquire) & 1U)) return false;
  const uint32_t ip = this->remote_rtp_ip_v4_.load(std::memory_order_acquire);
  const uint16_t port = this->remote_rtp_port_.load(std::memory_order_acquire);
  if (ip == 0 || port == 0) return false;
  uint8_t packet[16]{}; packet[0] = 0x80; packet[1] = format.payload_type | (report.marker ? 0x80 : 0);
  const uint16_t sequence = separate_sequence != nullptr ? (*separate_sequence)++ : this->rtp_sequence_.fetch_add(1);
  if (separate_sequence == nullptr) ssrc = this->rtp_ssrc_;
  packet[2] = sequence >> 8; packet[3] = sequence;
  packet[4] = report.timestamp >> 24; packet[5] = report.timestamp >> 16;
  packet[6] = report.timestamp >> 8; packet[7] = report.timestamp;
  packet[8] = ssrc >> 24; packet[9] = ssrc >> 16; packet[10] = ssrc >> 8; packet[11] = ssrc;
  packet[12] = report.event; packet[13] = 10 | (report.end ? 0x80 : 0);
  packet[14] = report.duration >> 8; packet[15] = report.duration;
  sockaddr_in target{}; target.sin_family = AF_INET; target.sin_addr.s_addr = htonl(ip); target.sin_port = htons(port);
  const int sent = sendto(this->rtp_socket_, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&target), sizeof(target));
  if (sent == static_cast<int>(sizeof(packet))) {
    this->rtp_tx_packets_.fetch_add(1); this->rtp_tx_bytes_.fetch_add(sizeof(packet)); return true;
  }
  return false;
}
}  // namespace esphome::voip_stack
#endif
