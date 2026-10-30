#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace esphome::voip_stack {

struct DtmfFormat {
  uint32_t clock{0};
  uint16_t events{0};
  uint8_t payload_type{0};
  bool valid() const { return this->clock != 0 && this->events != 0 && this->payload_type >= 96 && this->payload_type <= 127; }
};
inline constexpr uint32_t DTMF_CLOCKS[]{8000, 12000, 16000, 24000, 32000, 44100, 48000};
inline int dtmf_clock_index(uint32_t clock) {
  for (unsigned i = 0; i < 7; ++i) if (DTMF_CLOCKS[i] == clock) return i;
  return -1;
}
inline int dtmf_event(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit == '*') return 10;
  if (digit == '#') return 11;
  if (digit >= 'A' && digit <= 'D') return 12 + digit - 'A';
  if (digit >= 'a' && digit <= 'd') return 12 + digit - 'a';
  return -1;
}
inline char dtmf_digit(uint8_t event) {
  return event < 10 ? '0' + event : event == 10 ? '*' : event == 11 ? '#' : 'A' + event - 12;
}
inline std::string_view dtmf_trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
  return value;
}
inline bool dtmf_number(std::string_view text, uint32_t limit, uint32_t *out) {
  if (text.empty()) return false;
  uint32_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9' || result > (limit - static_cast<unsigned>(c - '0')) / 10) return false;
    result = result * 10 + c - '0';
    if (result > limit) return false;
  }
  *out = result; return true;
}
inline bool dtmf_rtpmap(std::string_view line, DtmfFormat *format) {
  constexpr std::string_view prefix = "a=rtpmap:";
  if (line.substr(0, prefix.size()) != prefix) return false;
  line.remove_prefix(prefix.size());
  const auto space = line.find_first_of(" \t"); uint32_t pt = 0;
  if (space == std::string_view::npos || !dtmf_number(line.substr(0, space), 127, &pt) || pt < 96) return false;
  line = dtmf_trim(line.substr(space + 1));
  constexpr std::string_view codec = "telephone-event/";
  if (line.size() < codec.size()) return false;
  for (size_t i = 0; i < codec.size(); ++i) {
    char c = line[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != codec[i]) return false;
  }
  line.remove_prefix(codec.size());
  const auto channels = line.find('/'); uint32_t clock = 0;
  if (channels != std::string_view::npos && line.substr(channels) != "/1") return false;
  if (!dtmf_number(line.substr(0, channels), 48000, &clock) || dtmf_clock_index(clock) < 0) return false;
  *format = {clock, 0xFFFF, static_cast<uint8_t>(pt)}; return true;
}
inline bool dtmf_event_mask(std::string_view text, uint16_t *mask) {
  uint16_t result = 0; unsigned fields = 0;
  while (!text.empty()) {
    if (++fields > 32) return false;
    const auto comma = text.find(','); const auto part = dtmf_trim(text.substr(0, comma));
    const auto dash = part.find('-'); uint32_t first = 0, last = 0;
    if (!dtmf_number(part.substr(0, dash), 255, &first)) return false;
    last = first;
    if (dash != std::string_view::npos && (!dtmf_number(part.substr(dash + 1), 255, &last) || last < first)) return false;
    for (uint32_t i = first; i <= std::min<uint32_t>(last, 15); ++i) result |= 1U << i;
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1); if (text.empty()) return false;
  }
  *mask = result; return result != 0;
}
// The selected audio m-line owns both PT presence and preference. Attributes
// may occur in any order. A missing fmtp retains RFC4733's default events0-15.
inline DtmfFormat negotiate_dtmf(std::string_view media, const uint8_t *ranks,
                                 uint32_t preferred_clock, bool answer, uint8_t offered_clocks) {
  DtmfFormat best; unsigned best_score = 1000;
  for (size_t start = 0; start < media.size();) {
    const auto end = media.find('\n', start); const auto line = dtmf_trim(media.substr(start, end - start));
    DtmfFormat candidate;
    if (dtmf_rtpmap(line, &candidate) && ranks[candidate.payload_type] != 255) {
      const int index = dtmf_clock_index(candidate.clock);
      bool valid = !answer || (offered_clocks & (1U << index));
      bool seen_fmtp = false;
      for (size_t other = 0; valid && other < media.size();) {
        const auto last = media.find('\n', other); const auto attribute = dtmf_trim(media.substr(other, last - other));
        DtmfFormat duplicate;
        if (dtmf_rtpmap(attribute, &duplicate) && duplicate.payload_type == candidate.payload_type && duplicate.clock != candidate.clock) valid = false;
        if (attribute.substr(0, 7) == "a=fmtp:") {
          const auto space = attribute.find_first_of(" \t", 7); uint32_t pt = 0;
          if (space != std::string_view::npos && dtmf_number(attribute.substr(7, space - 7), 127, &pt) && pt == candidate.payload_type) {
            uint16_t mask = 0;
            if (!dtmf_event_mask(dtmf_trim(attribute.substr(space + 1)), &mask) || (seen_fmtp && mask != candidate.events)) valid = false;
            candidate.events = mask; seen_fmtp = true;
          }
        }
        if (last == std::string_view::npos) break;
        other = last + 1;
      }
      const unsigned score = (candidate.clock == preferred_clock ? 0 : candidate.clock == 8000 ? 128 : 256) + ranks[candidate.payload_type];
      if (valid && candidate.valid() && score < best_score) { best = candidate; best_score = score; }
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return best;
}

class DtmfSender {
 public:
  struct Report { uint32_t timestamp; uint16_t duration; uint8_t event; bool marker, end; };
  bool start(uint8_t event, uint16_t duration_ms, const DtmfFormat &format,
              uint32_t timestamp, uint32_t epoch, uint32_t now) {
    const uint64_t ticks = uint64_t(duration_ms) * format.clock / 1000;
    if (event > 15 || !(format.events & (1U << event)) || this->active_ || duration_ms < 40 || duration_ms > 8000 || ticks == 0 || ticks > 65535 || !format.valid()) return false;
    this->format = format; this->epoch = epoch; this->timestamp_ = timestamp;
    this->event_ = event; this->duration_ms_ = duration_ms; this->started_ = now;
    this->next_ = now + 20; this->end_count_ = 0; this->first_ = this->active_ = true; return true;
  }
  bool poll(uint32_t now, Report *out) {
    if (!this->active_ || int32_t(now - this->next_) < 0) return false;
    const uint32_t elapsed = now - this->started_;
    const bool end = elapsed >= this->duration_ms_;
    *out = {this->timestamp_, static_cast<uint16_t>(uint64_t(std::min<uint32_t>(elapsed, this->duration_ms_)) * this->format.clock / 1000), this->event_, this->first_, end};
    this->first_ = false;
    if (end && this->end_count_++ == 0) this->ended_ = now;
    this->next_ = now + 20;
    if (this->end_count_ == 3) { this->active_ = false; this->next_ = uint32_t(this->ended_ + 70); }
    return true;
  }
  void cancel() { this->active_ = false; this->next_ = 0; this->end_count_ = 0; }
  bool active() const { return this->active_; }
  bool ready(uint32_t now) const { return !this->active_ && (this->end_count_ != 3 || int32_t(now - this->next_) >= 0); }
  uint32_t next() const { return this->next_; }
  DtmfFormat format{};
  uint32_t epoch{0};
 private:
  uint32_t timestamp_{0}, started_{0}, next_{0}, ended_{0};
  uint16_t duration_ms_{0};
  uint8_t event_{0}, end_count_{0};
  bool active_{false}, first_{true};
};

// A validated media tuple may change SSRC without another offer. Keep a small
// recent-source window so delayed final reports cannot retrigger an old digit.
class DtmfReceiver {
 public:
  bool accept(uint32_t ssrc, uint32_t timestamp, uint8_t event, uint32_t epoch) {
    if (event > 15) return false;
    if (epoch != this->epoch_) { for (auto &source : this->sources_) source = {}; this->next_ = 0; this->epoch_ = epoch; }
    Source *found = nullptr;
    for (auto &source : this->sources_) if (source.valid && source.ssrc == ssrc) { found = &source; break; }
    if (found == nullptr) {
      found = &this->sources_[this->next_]; this->next_ = (this->next_ + 1) % 4;
      *found = {ssrc, timestamp, 0, true};
    } else if (int32_t(timestamp - found->timestamp) < 0) return false;
    if (timestamp != found->timestamp) { found->timestamp = timestamp; found->events = 0; }
    const uint16_t mask = 1U << event;
    if (found->events & mask) return false;
    found->events |= mask; return true;
  }
 private:
  struct Source { uint32_t ssrc{0}, timestamp{0}; uint16_t events{0}; bool valid{false}; };
  Source sources_[4]{};
  uint32_t epoch_{0};
  uint8_t next_{0};
};
}  // namespace esphome::voip_stack
