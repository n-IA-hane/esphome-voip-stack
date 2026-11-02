#include "net_stream.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#ifdef USE_PHONE_CLIENT_TLS
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#endif

namespace esphome::phone_client {

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {
uint32_t remaining(uint64_t deadline) {
  const auto now = monotonic_ms();
  return now < deadline ? static_cast<uint32_t>(std::min<uint64_t>(deadline - now, UINT32_MAX)) : 0;
}
}

bool read_exact(Stream &stream, uint8_t *data, size_t length, uint32_t timeout_ms) {
  const uint64_t deadline = monotonic_ms() + timeout_ms;
  while (length != 0) {
    const auto wait = remaining(deadline);
    if (wait == 0) return false;
    const int count = stream.read(data, length, wait);
    if (count <= 0 || static_cast<size_t>(count) > length) return false;
    data += count;
    length -= static_cast<size_t>(count);
  }
  return true;
}

bool write_all(Stream &stream, const uint8_t *data, size_t length, uint32_t timeout_ms) {
  const uint64_t deadline = monotonic_ms() + timeout_ms;
  while (length != 0) {
    const auto wait = remaining(deadline);
    if (wait == 0) return false;
    const int count = stream.write(data, length, wait);
    if (count <= 0 || static_cast<size_t>(count) > length) return false;
    data += count;
    length -= static_cast<size_t>(count);
  }
  return true;
}

bool TcpStream::connect(const std::string &host, uint16_t port, uint32_t timeout_ms,
                        bool tls, const std::string &ca_pem) {
  this->close();
  this->interrupted_.store(false);
  if (host.empty() || port == 0 || timeout_ms == 0) return false;
  if (tls) {
#ifdef USE_PHONE_CLIENT_TLS
    auto *context = esp_tls_init();
    if (context == nullptr) return false;
    esp_tls_cfg_t cfg{};
    cfg.non_block = true;
    if (ca_pem.empty()) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    else {
      cfg.cacert_buf = reinterpret_cast<const unsigned char *>(ca_pem.c_str());
      cfg.cacert_bytes = ca_pem.size() + 1;
    }
    this->tls_ = context;
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (!this->interrupted_.load() && remaining(deadline) != 0) {
      // IDF may wait for TCP readiness inside one async step. Bound that
      // slice while preserving one overall caller deadline.
      cfg.timeout_ms = static_cast<int>(std::min<uint32_t>(remaining(deadline), 25));
      const int result = esp_tls_conn_new_async(host.c_str(), host.size(), port, &cfg, context);
      int fd = -1;
      if (esp_tls_get_conn_sockfd(context, &fd) == ESP_OK && fd >= 0) {
        std::lock_guard<std::mutex> lease(this->fd_mutex_);
        this->fd_.store(fd);
      }
      if (this->interrupted_.load() || result < 0) break;
      if (result == 1) return true;
      // Async handshake does not expose WANT_READ versus WANT_WRITE. Read
      // readiness or a short timeout retries both without a writable spin.
      if (fd >= 0) this->wait_(false, std::min<uint32_t>(remaining(deadline), 25));
    }
    this->close();
    return false;
#else
    (void) ca_pem;
    return false;
#endif
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) return false;
  bool connected = false;
  const uint64_t deadline = monotonic_ms() + timeout_ms;
  for (auto *address = addresses; address != nullptr && remaining(deadline) != 0 && !this->interrupted_.load();
       address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) continue;
    {
      std::lock_guard<std::mutex> lease(this->fd_mutex_);
      this->fd_.store(fd);
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { this->close(); continue; }
    const int result = ::connect(fd, address->ai_addr, address->ai_addrlen);
    if (result == 0 || (errno == EINPROGRESS && this->wait_(true, remaining(deadline)))) {
      int error = 0;
      socklen_t size = sizeof(error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) connected = true;
    }
    if (connected && !this->interrupted_.load()) break;
    connected = false;
    this->close();
  }
  ::freeaddrinfo(addresses);
  return connected;
}

bool TcpStream::wait_(bool write, uint32_t timeout_ms) const {
  const int fd = this->fd_.load();
  if (fd < 0 || fd >= FD_SETSIZE) return false;
  const uint64_t deadline = monotonic_ms() + timeout_ms;
  for (;;) {
    const uint32_t wait = remaining(deadline);
    if (wait == 0) return false;
    timeval tv{static_cast<long>(wait / 1000), static_cast<long>((wait % 1000) * 1000)};
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    const int result = ::select(fd + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
    if (result < 0 && errno == EINTR) continue;
    return result > 0 && this->fd_.load() == fd;
  }
}

int TcpStream::read(uint8_t *data, size_t length, uint32_t timeout_ms) {
  if (length == 0) return 0;
#ifdef USE_PHONE_CLIENT_TLS
  if (this->tls_ != nullptr) {
    auto *tls = static_cast<esp_tls_t *>(this->tls_);
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
      // Plaintext may already be buffered by TLS while its socket is quiet.
      const int count = esp_tls_conn_read(tls, data, length);
      if (count >= 0) return count;
      if (count != ESP_TLS_ERR_SSL_WANT_READ && count != ESP_TLS_ERR_SSL_WANT_WRITE) return count;
      if (!this->wait_(count == ESP_TLS_ERR_SSL_WANT_WRITE, remaining(deadline))) return -1;
    }
  }
#endif
  if (!this->wait_(false, timeout_ms)) return -1;
  return ::recv(this->fd_.load(), data, std::min<size_t>(length, INT_MAX), 0);
}

int TcpStream::write(const uint8_t *data, size_t length, uint32_t timeout_ms) {
  if (length == 0) return 0;
#ifdef USE_PHONE_CLIENT_TLS
  if (this->tls_ != nullptr) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
      const int count = esp_tls_conn_write(static_cast<esp_tls_t *>(this->tls_), data, length);
      if (count >= 0) return count;
      if (count != ESP_TLS_ERR_SSL_WANT_READ && count != ESP_TLS_ERR_SSL_WANT_WRITE) return count;
      if (!this->wait_(count == ESP_TLS_ERR_SSL_WANT_WRITE, remaining(deadline))) return -1;
    }
  }
#endif
  if (!this->wait_(true, timeout_ms)) return -1;
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  return ::send(this->fd_.load(), data, std::min<size_t>(length, INT_MAX), flags);
}

void TcpStream::interrupt() {
  this->interrupted_.store(true);
  std::lock_guard<std::mutex> lease(this->fd_mutex_);
  const int fd = this->fd_.load();
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

void TcpStream::close() {
  int fd;
  {
    std::lock_guard<std::mutex> lease(this->fd_mutex_);
    fd = this->fd_.exchange(-1);
  }
#ifdef USE_PHONE_CLIENT_TLS
  if (this->tls_ != nullptr) {
    esp_tls_conn_destroy(static_cast<esp_tls_t *>(this->tls_));
    this->tls_ = nullptr;
    return;
  }
#endif
  if (fd >= 0) ::close(fd);
}

}  // namespace esphome::phone_client
