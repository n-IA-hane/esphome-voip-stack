#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/time.h>
#define protected public
#include "video_rtp.h"
#undef protected
namespace esphome::voip_stack {
static uint32_t millis() { return 1500; }
static unsigned feedback_count;
/* PRODUCTION */
VideoRtpSession::~VideoRtpSession() {}
void VideoRtpSession::wake_task_() {}
void VideoRtpSession::send_rtcp_packet_(const uint8_t *data, size_t size) {
  assert(size >= 12 && data[size - 12] == 0x81 && data[size - 11] == 206);
  ++feedback_count;
}
}
using namespace esphome::voip_stack;

class Sink final : public EncodedVideoSink {
 public:
  explicit Sink(std::string fault) : fault_(std::move(fault)) {}
  VideoCapability get_receive_video_capability() const override { return {}; }
  bool start_video(const VideoCapability &) override { return true; }
  bool set_video_active(bool) override { return true; }
  void stop_video() override {}
  bool consume_video_access_unit(const EncodedVideoAccessUnit &unit) override {
    if (fault_ == "sink-reject" && unit.timestamp_90khz == 9000) return false;
    std::printf("%u ", unsigned(unit.timestamp_90khz));
    for (size_t i = 0; i < unit.size; ++i) std::printf("%02x", unit.data[i]);
    std::puts("");
    if (fault_ == "no-slot" && unit.timestamp_90khz <= 18000)
      held_.push_back(unit);
    else
      unit.release();
    return true;
  }
  void release_held() {
    for (const auto &unit : held_) unit.release();
    held_.clear();
  }
 private:
  std::string fault_;
  std::vector<EncodedVideoAccessUnit> held_;
};

int main(int argc, char **argv) {
  assert(argc == 4);
  VideoRtpSession session(false);
  Sink sink(argv[2]);
  std::array<std::array<uint8_t, 512>, VideoRtpSession::kReceiveAccessUnitSlots> storage{};
  for (size_t i = 0; i < storage.size(); ++i) {
    session.receive_access_units_[i].owner = &session;
    session.receive_access_units_[i].data = storage[i].data();
  }
  session.receive_enabled_ = true;
  session.capability_.payload_type = 105;
  session.capability_.encoding = "H264";
  session.send_remote_pli_ = std::atoi(argv[3]) != 0;
  session.remote_ssrc_ = 1;
  session.receive_access_unit_capacity_ = storage.front().size();
  session.sink_ = &sink;
  uint8_t packet[512];
  for (;;) {
    const auto n = recv(std::atoi(argv[1]), packet, sizeof(packet), MSG_DONTWAIT);
    if (n < 0) break;
    session.handle_rtp_packet_(packet, n);
    if (esphome::voip_stack::read_be32(packet + 4) == 27000)
      sink.release_held();
  }
  sink.release_held();
  for (const auto &slot : session.receive_access_units_) assert(slot.state != 2);
  std::fprintf(stderr, "%u\n", feedback_count);
}
