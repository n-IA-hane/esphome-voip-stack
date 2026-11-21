#include "sip_udp_receive.h"

#include <array>
#include <cassert>
#include <string>
#include <unistd.h>

using namespace esphome::voip_stack;

int main() {
  const int receiver = socket(AF_INET, SOCK_DGRAM, 0);
  const int sender = socket(AF_INET, SOCK_DGRAM, 0);
  assert(receiver >= 0 && sender >= 0);
  sockaddr_in destination{}; destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(receiver, reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) == 0);
  socklen_t size = sizeof(destination);
  assert(getsockname(receiver, reinterpret_cast<sockaddr *>(&destination), &size) == 0);
  std::array<char, 2048> buffer{};
  const std::string header = "NOTIFY sip:phone@127.0.0.1 SIP/2.0\r\nContent-Length: 0\r\n\r\n";
  for (const size_t bytes : {header.size(), buffer.size()-1, buffer.size(), buffer.size()+512}) {
    std::string message = header + std::string(bytes-header.size(), 'x');
    assert(sendto(sender, message.data(), message.size(), 0,
                  reinterpret_cast<sockaddr *>(&destination), size) == static_cast<ssize_t>(message.size()));
    sockaddr_in source{};
    const auto received = receive_sip_udp(receiver, buffer.data(), buffer.size(), &source);
    assert(received.truncated == (bytes >= buffer.size()));
    if (!received.truncated) {
      assert(received.bytes == static_cast<int>(bytes) && buffer[bytes] == '\0');
      assert(std::string(buffer.data(), bytes) == message);
    }
  }
  close(sender); close(receiver);
}
