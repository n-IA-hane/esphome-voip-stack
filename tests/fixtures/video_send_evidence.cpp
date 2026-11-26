#include "video_rtp.h"
#include "transport_host_sdk.h"
#include <sys/socket.h>
#include <unistd.h>

using namespace esphome::voip_stack;

struct VideoProbe : VideoRtpSession {
  VideoProbe() : VideoRtpSession(false) {}
  void exercise() {
    rtp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    assert(rtp_socket_ >= 0);
    running_.store(true);
    send_enabled_.store(true);
    remote_ip_v4_.store(INADDR_LOOPBACK);
    remote_rtp_port_.store(0);  // Real sendto failure, without debug firmware.
    uint8_t packet[13]{};
    packet[12] = 0x61;
    assert(!send_rtp_packet_(packet, 1, true, 90000));
    assert(tx_send_failures_.load() == 1 && tx_packets_.load() == 0);

    const int peer = socket(AF_INET, SOCK_DGRAM, 0);
    assert(peer >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(peer, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(peer, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    remote_rtp_port_.store(ntohs(address.sin_port));
    assert(send_rtp_packet_(packet, 1, true, 180000));
    uint8_t received[32]{};
    assert(recv(peer, received, sizeof(received), 0) == 13);
    assert(received[0] == 0x80 && received[12] == 0x61);
    assert(tx_send_failures_.load() == 1 && tx_packets_.load() == 1 && tx_octets_.load() == 1);
    close(peer);
    close(rtp_socket_);
    rtp_socket_ = -1;
    running_.store(false);
    send_enabled_.store(false);
  }
};

int main() {
  VideoProbe probe;
  probe.exercise();
}
