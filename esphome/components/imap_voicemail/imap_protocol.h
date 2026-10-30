#pragma once

#include "esphome/components/phone_client/net_stream.h"
#include "esphome/components/phone_client/buffer_allocator.h"
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace esphome::imap_voicemail {

struct AudioPart {
  std::string section, mime, encoding;
  uint32_t encoded_size{0};
  bool valid() const { return !section.empty() && !mime.empty(); }
};
struct Message {
  uint32_t uid{0}, uid_validity{0}, size{0};
  uint32_t duration_seconds{0};
  bool has_duration{false};
  bool seen{false}, deleted{false};
  std::string date, from, subject;
  AudioPart audio;
};
struct Mailbox {
  uint32_t uid_validity{0}, total{0};
  std::vector<Message, phone_client::BufferAllocator<Message>> messages;
  Mailbox() : Mailbox(true) {}
  explicit Mailbox(bool external) : messages(phone_client::BufferAllocator<Message>(external)) {}
};
struct Value {
  enum class Kind : uint8_t { ATOM, STRING, LIST, LITERAL } kind{Kind::ATOM};
  std::string text;
  std::vector<Value> children;
  size_t literal{0};
};
struct Response {
  std::string text;
  struct Literal { size_t offset{0}, size{0}; };
  std::array<Literal, 8> literals{};
  size_t count{0}, bytes{0};
};

bool parse_values(const Response &response, const uint8_t *literal_data, std::vector<Value> *values);
std::string value_text(const Value &value, const Response &response, const uint8_t *literal_data);
bool parse_bodystructure(const Value &body, AudioPart *audio);
std::string quote(const std::string &value);
bool parse_uid(const std::string &value, uint32_t *uid);
bool parse_content_duration(const std::string &headers, uint32_t *duration);
std::string decode_header(std::string text);
std::string mailbox_wire_name(const std::string &utf8);
std::string make_message_uri(const std::string &host, uint16_t port, const std::string &folder,
                             uint32_t validity, uint32_t uid);
bool parse_message_uri(const std::string &uri, const std::string &host, uint16_t port,
                       const std::string &folder, uint32_t *validity, uint32_t *uid);

// One bounded IMAP session. Metadata/IDLE and FETCH each own a separate
// connection and caller-owned literal buffer; no HTTP or SIP media relay.
class Client {
 public:
  Client(phone_client::Stream &stream, uint8_t *literal_buffer, size_t literal_capacity);
  bool login(const std::string &username, const std::string &password, uint32_t timeout_ms);
  bool select(const std::string &folder, uint32_t timeout_ms);
  bool catalogue(size_t maximum, size_t page, Mailbox *mailbox, uint32_t timeout_ms);
  bool fetch_audio(uint32_t uid, const AudioPart &audio, uint32_t offset, size_t maximum,
                   size_t *received, uint32_t timeout_ms);
  void set_fetch_buffer(uint8_t *buffer, size_t size) { this->fetch_ = buffer; this->fetch_capacity_ = size; }
  void set_buffer_in_psram(bool value) { this->buffer_in_psram_ = value; }
  bool store_seen(uint32_t uid, bool seen, uint32_t timeout_ms);
  bool delete_uid(uint32_t uid, uint32_t timeout_ms);
  bool start_idle(uint32_t timeout_ms);
  bool wait_idle(uint32_t timeout_ms, bool *changed);
  bool end_idle(uint32_t timeout_ms);
  void logout(uint32_t timeout_ms);
  bool supports_idle() const { return this->idle_supported_; }
  bool supports_uidplus() const { return this->uidplus_; }
  uint32_t uid_validity() const { return this->uid_validity_; }
  const uint8_t *fetched_data() const { return this->fetch_; }
  const std::string &error() const { return this->error_; }

 protected:
  bool line_(std::string *line, uint64_t deadline);
  bool exact_(uint8_t *data, size_t length, uint64_t deadline);
  bool response_(Response *response, uint32_t timeout_ms);
  bool command_(const std::string &command, uint32_t timeout_ms,
                const std::function<bool(const Response &)> &untagged = {});
  bool send_(const std::string &value, uint32_t timeout_ms);
  std::string tag_();
  phone_client::Stream &stream_;
  uint8_t *literal_;
  size_t literal_capacity_;
  uint8_t *fetch_{nullptr};
  size_t fetch_capacity_{0};
  std::array<uint8_t, 1024> input_{};
  size_t input_at_{0}, input_size_{0};
  uint32_t next_tag_{1}, uid_validity_{0}, exists_{0};
  bool idle_supported_{false}, uidplus_{false}, idling_{false}, pending_change_{false};
  bool buffer_in_psram_{true};
  std::string idle_tag_, error_;
};

// Incremental MIME transfer decoding; state survives arbitrary IMAP partial
// fetch boundaries. Call finish() only at the encoded body-part end.
class TransferDecoder {
 public:
  using Sink = std::function<bool(const uint8_t *, size_t)>;
  explicit TransferDecoder(std::string encoding);
  bool feed(const uint8_t *data, size_t length, const Sink &sink);
  bool finish();
 protected:
  std::string encoding_;
  std::array<uint8_t, 4> pending_{};
  uint8_t count_{0};
  bool ended_{false}, failed_{false};
};

}  // namespace esphome::imap_voicemail
