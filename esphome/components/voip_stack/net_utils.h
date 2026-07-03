#pragma once

#include <cstdint>
#include <cerrno>
#include <cstring>
#include <sys/select.h>

namespace esphome {
namespace voip_stack {

static inline timeval make_timeval_ms(uint32_t timeout_ms) {
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return tv;
}

static inline bool wait_socket_readable(int socket, uint32_t timeout_ms) {
  if (socket < 0) return false;
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(socket, &fds);
  timeval tv = make_timeval_ms(timeout_ms);
  int ret = ::select(socket + 1, &fds, nullptr, nullptr, &tv);
  return ret > 0 && FD_ISSET(socket, &fds);
}

static inline bool wait_socket_writable(int socket, uint32_t timeout_ms) {
  if (socket < 0) return false;
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(socket, &fds);
  timeval tv = make_timeval_ms(timeout_ms);
  int ret = ::select(socket + 1, nullptr, &fds, nullptr, &tv);
  return ret > 0 && FD_ISSET(socket, &fds);
}

static inline const char *socket_errno_name(int err) {
  switch (err) {
    case EACCES:
      return "EACCES";
    case EADDRINUSE:
      return "EADDRINUSE";
    case EADDRNOTAVAIL:
      return "EADDRNOTAVAIL";
    case ECONNABORTED:
      return "ECONNABORTED";
    case ECONNREFUSED:
      return "ECONNREFUSED";
    case ECONNRESET:
      return "ECONNRESET";
    case EINPROGRESS:
      return "EINPROGRESS";
    case EINTR:
      return "EINTR";
    case EINVAL:
      return "EINVAL";
    case ENETDOWN:
      return "ENETDOWN";
    case ENETUNREACH:
      return "ENETUNREACH";
    case ENOBUFS:
      return "ENOBUFS";
    case ENOTCONN:
      return "ENOTCONN";
    case ETIMEDOUT:
      return "ETIMEDOUT";
    case EAGAIN:
      return "EAGAIN";
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
      return "EWOULDBLOCK";
#endif
    default:
      return "UNKNOWN";
  }
}

static inline const char *socket_errno_text(int err) { return strerror(err); }

}  // namespace voip_stack
}  // namespace esphome
