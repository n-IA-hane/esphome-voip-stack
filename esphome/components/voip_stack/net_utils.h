#pragma once

#include <cstdint>
#include <cerrno>
#include <cstring>

namespace esphome {
namespace voip_stack {

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
