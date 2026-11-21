#pragma once

#include <array>
#include <cstdint>

namespace esphome::voip_stack {

enum class VideoPayloadMeaning : uint8_t { UNDECLARED, JPEG, H264, OTHER };

// One lifetime per receiving RTP stream. Two bits retain each used meaning,
// including retired payloads, without allocating a 128-entry string table.
struct VideoPayloadContract {
  std::array<uint8_t, 32> tx_meanings{}, rx_meanings{};
  uint8_t tx_pt{0}, rx_pt{0};
  bool conflict{false};

  static VideoPayloadMeaning meaning(const std::array<uint8_t, 32> &history, uint8_t pt) {
    return static_cast<VideoPayloadMeaning>((history[pt / 4] >> ((pt % 4) * 2)) & 3);
  }
  static bool permits(const std::array<uint8_t, 32> &history, uint8_t pt, VideoPayloadMeaning next) {
    const auto previous = meaning(history, pt);
    return previous == VideoPayloadMeaning::UNDECLARED || previous == next;
  }
  bool remember(uint8_t tx, uint8_t rx, VideoPayloadMeaning codec) {
    if (tx > 127 || rx > 127 || codec == VideoPayloadMeaning::UNDECLARED || codec == VideoPayloadMeaning::OTHER ||
        !permits(this->tx_meanings, tx, codec) || !permits(this->rx_meanings, rx, codec)) return false;
    this->tx_pt = tx; this->rx_pt = rx;
    const auto store = [codec](auto &history, uint8_t pt) {
      const auto shift = (pt % 4) * 2;
      history[pt / 4] = (history[pt / 4] & ~(3U << shift)) | (static_cast<uint8_t>(codec) << shift);
    };
    store(this->tx_meanings, tx); store(this->rx_meanings, rx);
    return true;
  }
};

}  // namespace esphome::voip_stack
