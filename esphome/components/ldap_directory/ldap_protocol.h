#pragma once

#include "esphome/components/phone_client/net_stream.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace esphome::ldap_directory {

struct Attribute {
  std::string name;
  std::vector<std::string> values;
};
struct Entry {
  std::string dn;
  std::vector<Attribute> attributes;
  std::string first(const std::string &name) const;
};
struct Search {
  std::string base_dn;
  std::string filter{"(objectClass=*)"};
  std::vector<std::string> attributes{"uid", "entryUUID", "cn", "displayName", "telephoneNumber", "labeledURI", "ou"};
  uint32_t size_limit{64};
  uint32_t time_limit{3};
  uint8_t scope{2};
};
struct Result {
  uint32_t message_id{0};
  uint8_t operation{0};
  uint32_t code{0};
  Entry entry;
};

std::string escape_dn_value(const std::string &value);
bool valid_filter(const std::string &filter);
bool encode_bind(uint32_t id, const std::string &dn, const std::string &password, std::string *packet);
bool encode_search(uint32_t id, const Search &search, std::string *packet);
std::string encode_unbind(uint32_t id);
bool decode_message(std::string_view packet, Result *result, size_t max_attributes = 16, size_t max_values = 16);
// One framed LDAPMessage. Definite BER lengths only; a peer cannot grow the
// receive buffer past the caller's announced maximum.
bool read_message(phone_client::Stream &stream, std::string *packet, size_t limit, uint32_t timeout_ms);
bool read_message(phone_client::Stream &stream, char *buffer, size_t capacity, size_t *length, uint32_t timeout_ms);

class Client {
 public:
  explicit Client(phone_client::Stream &stream, size_t max_packet = 8192) : stream_(stream), max_packet_(max_packet) {}
  void set_receive_buffer(char *buffer, size_t size) { this->receive_buffer_ = buffer; this->max_packet_ = size; }
  bool bind(const std::string &dn, const std::string &password, uint32_t timeout_ms);
  bool search(const Search &search, std::vector<Entry> *entries, uint32_t timeout_ms);
  // Entries arrive one at a time; consumers may move their owned values.
  // A successful callback does not commit the directory: only a final
  // SearchResultDone SUCCESS does. No entry reference outlives its callback.
  bool search_each(const Search &search, const std::function<bool(Entry &&)> &consume, uint32_t timeout_ms);
  void unbind(uint32_t timeout_ms);
  uint32_t result_code() const { return this->result_code_; }

 protected:
  phone_client::Stream &stream_;
  size_t max_packet_;
  uint32_t next_id_{1}, result_code_{0};
  std::string packet_;
  char *receive_buffer_{nullptr};
  bool receive_(Result *result, uint32_t timeout_ms);
};

}  // namespace esphome::ldap_directory
