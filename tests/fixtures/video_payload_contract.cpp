#include "media_negotiator.h"
#include "sip_sdp_video_attributes.h"

#include <cassert>

using namespace esphome::voip_stack;

VideoOfferAttributes description(const std::string &payloads, const std::string &mappings, uint16_t port = 42000) {
  VideoOfferAttributes result; VideoCapability local;
  read_video_offer_attributes("v=0\r\nc=IN IP4 192.0.2.2\r\nm=video " + std::to_string(port) +
                              " RTP/AVP " + payloads + "\r\n" + mappings, 0, local, local, result);
  return result;
}

int main() {
  static_assert(sizeof(VideoPayloadContract) == 67);
  VideoPayloadContract empty, active;
  const auto answer = description("120", "a=rtpmap:120 H264/90000\r\n");
  assert(negotiate_video_payload_contract(answer, true, 103, 120, empty, &active));
  assert(active.tx_pt == 120 && active.rx_pt == 103);
  assert(VideoPayloadContract::meaning(active.tx_meanings, 120) == VideoPayloadMeaning::H264);
  assert(VideoPayloadContract::meaning(active.rx_meanings, 103) == VideoPayloadMeaning::H264);
  // An answer's transmit namespace is independent from our receive namespace.
  assert(VideoPayloadContract::meaning(active.rx_meanings, 120) == VideoPayloadMeaning::UNDECLARED);
  const auto removed = description("120", "", 0);
  assert(removed.media_port == 0 && video_payload_history_allows(removed, false, active));
  auto after_removal = active;
  const auto later = description("121", "a=rtpmap:121 H264/90000\r\n");
  assert(negotiate_video_payload_contract(later, false, 103, 121, after_removal, &after_removal));
  assert(after_removal.tx_pt == 121 && after_removal.rx_pt == 121);
  assert(VideoPayloadContract::meaning(after_removal.tx_meanings, 120) == VideoPayloadMeaning::H264);
  const auto remapped = description("120 121", "a=rtpmap:120 JPEG/90000\r\na=rtpmap:121 H264/90000\r\n");
  assert(!video_payload_history_allows(remapped, false, after_removal));
  VideoPayloadContract rejected = after_removal;
  assert(!negotiate_video_payload_contract(remapped, false, 103, 121, after_removal, &rejected));
  assert(rejected.tx_pt == after_removal.tx_pt && rejected.tx_meanings == after_removal.tx_meanings);
  const auto changed_clock = description("103 121", "a=rtpmap:103 H264/8000\r\na=rtpmap:121 H264/90000\r\n");
  assert(!video_payload_history_allows(changed_clock, false, after_removal));
  assert(video_payload_history_allows(remapped, false, empty));
}
