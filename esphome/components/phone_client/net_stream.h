#pragma once

#if __has_include("esphome/core/defines.h")
#include "esphome/core/defines.h"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome::phone_client {

uint64_t monotonic_ms();

// A protocol owns its connection. interrupt() may be used by its control
// thread; only the connection owner calls close() or destroys TLS state.
class Stream {
 public:
  virtual ~Stream() = default;
  virtual int read(uint8_t *data, size_t length, uint32_t timeout_ms) = 0;
  virtual int write(const uint8_t *data, size_t length, uint32_t timeout_ms) = 0;
  virtual void interrupt() = 0;
  virtual void close() = 0;
};

bool read_exact(Stream &stream, uint8_t *data, size_t length, uint32_t timeout_ms);
bool write_all(Stream &stream, const uint8_t *data, size_t length, uint32_t timeout_ms);
inline bool write_all(Stream &stream, const std::string &data, uint32_t timeout_ms) {
  return write_all(stream, reinterpret_cast<const uint8_t *>(data.data()), data.size(), timeout_ms);
}

class TcpStream final : public Stream {
 public:
  ~TcpStream() override { this->close(); }
  bool connect(const std::string &host, uint16_t port, uint32_t timeout_ms,
               bool tls = false, const std::string &ca_pem = {});
  int read(uint8_t *data, size_t length, uint32_t timeout_ms) override;
  int write(const uint8_t *data, size_t length, uint32_t timeout_ms) override;
  void interrupt() override;
  void close() override;

 protected:
  bool wait_(bool write, uint32_t timeout_ms) const;
  std::atomic<int> fd_{-1};
#ifdef USE_PHONE_CLIENT_TLS
  void *tls_{nullptr};
#endif
};

}  // namespace esphome::phone_client
