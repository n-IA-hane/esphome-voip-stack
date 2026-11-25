#pragma once

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

// Keep the exact protocols owned until the fixture is ready to hand the port
// to production bind(). A UDP-only probe cannot reserve a SIP/TCP listener.
// Production has no socket-adoption API: release immediately before start.
class ReservedPorts {
 public:
  enum class Kind { UDP, SIP, RTP_PAIR };
  explicit ReservedPorts(Kind kind) {
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      first_ = socket(AF_INET, kind == Kind::SIP ? SOCK_STREAM : SOCK_DGRAM, 0);
      assert(first_ >= 0);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_ANY);
      assert(bind(first_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
      socklen_t size = sizeof(address);
      assert(getsockname(first_, reinterpret_cast<sockaddr *>(&address), &size) == 0);
      port_ = ntohs(address.sin_port);
      if (kind == Kind::UDP) return;
      if (kind == Kind::RTP_PAIR && ((port_ & 1U) || port_ == 65535)) { release(); continue; }
      second_ = socket(AF_INET, SOCK_DGRAM, 0);
      assert(second_ >= 0);
      if (kind == Kind::RTP_PAIR) address.sin_port = htons(port_ + 1);
      if (bind(second_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) return;
      release();
    }
    std::fprintf(stderr, "Unable to reserve native fixture ports after 64 attempts (errno=%d)\n", errno);
    std::abort();
  }
  ReservedPorts(const ReservedPorts &) = delete;
  ReservedPorts &operator=(const ReservedPorts &) = delete;
  ~ReservedPorts() { release(); }
  uint16_t port() const { return port_; }
  void release() {
    if (first_ >= 0) close(first_);
    if (second_ >= 0) close(second_);
    first_ = second_ = -1;
  }
 private:
  int first_{-1}, second_{-1};
  uint16_t port_{0};
};
