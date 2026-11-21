#pragma once

#include <cstddef>
#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace esphome::voip_stack {

struct SipUdpReceive {
  int bytes{-1};
  bool truncated{false};
};

// recvmsg exposes the complete datagram boundary on both lwIP and POSIX.
// lwIP can return the original length when only a prefix fitted the iovec.
inline SipUdpReceive receive_sip_udp(int socket, char *buffer, size_t capacity, sockaddr_in *source) {
  if (buffer == nullptr || source == nullptr || capacity == 0) return {};
  iovec part{buffer, capacity - 1};
  msghdr message{};
  message.msg_name = source;
  message.msg_namelen = sizeof(*source);
  message.msg_iov = &part;
  message.msg_iovlen = 1;
  const int count = recvmsg(socket, &message, 0);
  const bool truncated = (message.msg_flags & MSG_TRUNC) != 0 ||
                         (count >= 0 && static_cast<size_t>(count) > part.iov_len);
  if (count >= 0 && !truncated) buffer[count] = '\0';
  return {count, truncated};
}

}  // namespace esphome::voip_stack
