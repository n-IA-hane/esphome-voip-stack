#pragma once
#include <array>
#include <string_view>
#include <algorithm>
#include <cstring>
#include <cstdint>
namespace esphome::phone_list_view {
struct RowData {
  std::array<char, 161> id{};
  std::array<char, 129> name{};
  std::array<char, 129> detail{};
  std::array<char, 9> leading{};
  std::array<char, 9> trailing{};
  uint32_t color{0xCBD5E1};
  uint8_t action{0};
  bool enabled{true};
  bool blink{false};
  bool highlighted{false};

  template<size_t N> static void text(std::array<char, N> &out, std::string_view value) {
    size_t size = std::min(value.size(), N - 1);
    // Visible text may be clipped, but must remain valid UTF-8.
    if (size < value.size())
      while (size > 0 && (static_cast<uint8_t>(value[size]) & 0xC0) == 0x80) --size;
    if (size != 0) std::memcpy(out.data(), value.data(), size);
    out[size] = '\0';
  }
  void set_id(std::string_view value) {
    if (value.size() >= this->id.size()) { this->enabled = false; return; }
    text(this->id, value);
  }
  void set_name(std::string_view value) { text(this->name, value); }
  void set_detail(std::string_view value) { text(this->detail, value); }
  void set_leading(std::string_view value) { text(this->leading, value); }
  void set_trailing(std::string_view value) { text(this->trailing, value); }
};

// Borrowed main-loop views. Ordering never copies the source contact or its text.
struct RowKey {
  std::string_view name;
  std::string_view id;
};

}
