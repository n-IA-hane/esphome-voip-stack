#pragma once

#include "sip_sdp_video_attributes.h"

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO
namespace esphome::voip_stack {

enum class VideoOfferDisposition { DISABLED, REJECTED, SELECT_PAYLOAD };

inline VideoOfferDisposition validate_video_offer(VideoOfferAttributes &offer, uint32_t retained_ip,
                                                  uint32_t retained_rtcp_ip, bool allow_zero_connection,
                                                  uint8_t &payload_type, bool &zero_connection) {
  // Even a rejected stream must answer its m-line with a PT from the offer.
  if (offer.offered_payload_count != 0) payload_type = offer.offered_payload_order[0];
  if (!offer.offered) return VideoOfferDisposition::DISABLED;
  // A disabled answer is not an error and must not disturb audio.
  if (offer.media_port == 0) return VideoOfferDisposition::DISABLED;
  if (offer.media_zero_connection) {
    // Legacy hold applies to this media section after connection inheritance.
    // Keep the prepared child's peer identity for a direction-only pause.
    if (!allow_zero_connection) return VideoOfferDisposition::REJECTED;
    zero_connection = true;
    offer.media_direction = 0;
    if (retained_ip != 0) offer.media_ip = retained_ip;
    if (retained_rtcp_ip != 0) offer.media_rtcp_ip = retained_rtcp_ip;
  }
  // This implementation owns a distinct RTCP socket. Do not claim support for
  // a peer that requires multiplexing both protocols on one port.
  if (offer.rtcp_mux_only || offer.media_rtcp_attribute_invalid || offer.media_rtcp_port == 0 ||
      offer.media_rtcp_ip == 0) {
    return VideoOfferDisposition::REJECTED;
  }
  return VideoOfferDisposition::SELECT_PAYLOAD;
}

struct VideoPayloadSelection {
  VideoCapability &capability;
  bool send{false}, receive{false};
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
  bool imageattr_present{false}, imageattr_compatible{false};
#endif
};

inline bool select_video_payload(const VideoOfferAttributes &offer, const VideoCapability &local_send,
                                 const VideoCapability &local_receive, bool send_requested, bool remote_is_answer,
                                 uint8_t offered_pt, VideoPayloadSelection &selected) {
  for (size_t payload_index = 0; payload_index < offer.offered_payload_count; payload_index++) {
    const uint8_t pt = offer.offered_payload_order[payload_index];
    if (remote_is_answer && pt != offered_pt) continue;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    // Static PT 26 needs no rtpmap, but an explicit conflicting mapping wins.
    const bool jpeg = pt == 26 && (!offer.jpeg_mapping_seen || offer.jpeg_mapping_matches);
    const bool h264 = false;
#else
    const bool jpeg = false;
    const bool h264 = offer.h264_payloads[pt] && offer.packetization_modes[pt] == 1 && offer.profile_valid[pt];
#endif
    if (!jpeg && !h264) continue;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    const std::string peer_profile(offer.profiles[pt], 6);
#else
    const std::string peer_profile;
#endif
    const bool peer_sends = (offer.media_direction & 0x01) != 0;
    const bool peer_receives = (offer.media_direction & 0x02) != 0;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    const bool bilateral_asymmetry = offer.level_asymmetry_allowed[pt] &&
                                     (!peer_sends || local_receive.level_asymmetry_allowed) &&
                                     (!peer_receives || local_send.level_asymmetry_allowed);
#else
    const bool bilateral_asymmetry = false;
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    const bool send_compatible = local_send.valid() && jpeg && local_send.is_jpeg();
    const bool receive_compatible = local_receive.valid() && jpeg && local_receive.is_jpeg();
#else
    const bool send_compatible = local_send.valid() && h264 && local_send.is_h264() &&
                                 h264_level_fits(local_send.profile_level_id, peer_profile);
    const bool receive_compatible = local_receive.valid() && h264 && local_receive.is_h264() &&
                                    h264_same_subprofile(local_receive.profile_level_id, peer_profile);
#endif
    bool local_sends = send_requested && peer_receives && send_compatible;
    const bool local_receives = peer_sends && receive_compatible;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    if (!bilateral_asymmetry && local_sends && local_receives &&
        !h264_level_fits(local_send.profile_level_id, local_receive.profile_level_id))
      local_sends = false;
#endif
    const bool inactive_direction = offer.media_direction == 0;
    if (!local_sends && !local_receives && (!inactive_direction || (!send_compatible && !receive_compatible))) continue;

    // Keep the shared RTP capability stable across direction-only re-INVITEs.
    // Endpoint dimensions remain directional local metadata (RFC 2435 carries
    // JPEG frame dimensions in-band), so choose the same compatible endpoint
    // whether or not that direction is active in the current offer.
    VideoCapability &negotiated = selected.capability;
    negotiated = receive_compatible ? local_receive : local_send;
    // Keep the peer's recommendation on our encoder direction. The decoder
    // retains its own budget in local_video_direction_capability_().
    negotiated.max_fps = send_compatible ? local_send.max_fps : local_receive.max_fps;
    if (send_compatible && receive_compatible) {
      if (local_send.max_bitrate_bps != 0 &&
          (negotiated.max_bitrate_bps == 0 || local_send.max_bitrate_bps < negotiated.max_bitrate_bps)) {
        negotiated.max_bitrate_bps = local_send.max_bitrate_bps;
      }
    }
    if (offer.media_max_fps > 0) negotiated.max_fps = std::min(negotiated.max_fps, offer.media_max_fps);
    negotiated.payload_type = static_cast<uint8_t>(pt);
    negotiated.level_asymmetry_allowed = h264 && bilateral_asymmetry;
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    negotiated.rtcp_feedback_pli = offer.rtcp_feedback_pli[pt];
    negotiated.rtcp_feedback_fir = offer.rtcp_feedback_fir[pt];
#else
    negotiated.rtcp_feedback_pli = false;
    negotiated.rtcp_feedback_fir = false;
#endif
    if (offer.media_max_bitrate_bps != 0 &&
        (negotiated.max_bitrate_bps == 0 || offer.media_max_bitrate_bps < negotiated.max_bitrate_bps)) {
      negotiated.max_bitrate_bps = offer.media_max_bitrate_bps;
    }
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_H264
    if (!bilateral_asymmetry && h264_level_fits(peer_profile, negotiated.profile_level_id)) {
      // Lowering a decoder advertisement is safe; lowering the encoder SPS is
      // not. Preserve our profile_idc/profile_iop and select the common level.
      negotiated.profile_level_id.replace(4, 2, peer_profile.substr(4, 2));
    }
#endif
#ifdef USE_ESPHOME_VOIP_STACK_VIDEO_JPEG
    if (offer.imageattr_specific.valid || offer.imageattr_wildcard.valid) {
      selected.imageattr_present = true;
      const auto &send_attr =
          offer.imageattr_specific.send_present ? offer.imageattr_specific : offer.imageattr_wildcard;
      const auto &recv_attr =
          offer.imageattr_specific.recv_present ? offer.imageattr_specific : offer.imageattr_wildcard;
      selected.imageattr_compatible = (!local_receives || !send_attr.send_present || send_attr.send_compatible) &&
                                      (!local_sends || !recv_attr.recv_present || recv_attr.recv_compatible);
    }
#endif
    selected.send = local_sends;
    selected.receive = local_receives;
    return true;
  }
  return false;
}

}  // namespace esphome::voip_stack
#endif
