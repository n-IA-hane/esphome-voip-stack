#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::esp_video_renderer {

struct JpegGeometry {
  uint16_t width{0}, height{0};
};

// Bound every header segment before the vendor's skip/read callbacks see it.
// Only a complete, single-scan, 8-bit baseline image is admitted for decoding.
inline bool inspect_baseline_jpeg(const uint8_t *data, size_t size,
                                  uint16_t max_width, uint16_t max_height,
                                  JpegGeometry &geometry) {
  geometry = {};
  if (data == nullptr || size < 4 || data[0] != 0xff || data[1] != 0xd8)
    return false;
  size_t offset = 2;
  uint8_t components = 0;
  uint8_t ids[3]{};
  while (offset < size) {
    if (size - offset < 4 || data[offset] != 0xff)
      return false;
    const uint8_t marker = data[offset + 1];
    const size_t length = (size_t(data[offset + 2]) << 8) | data[offset + 3];
    if (length < 2 || length > size - offset - 2)
      return false;
    const uint8_t *segment = data + offset + 4;
    const size_t payload = length - 2;
    if (marker == 0xc0) {
      if (components != 0 || payload < 6 || segment[0] != 8)
        return false;
      components = segment[5];
      if ((components != 1 && components != 3) ||
          payload != 6U + 3U * components)
        return false;
      geometry.height = (uint16_t(segment[1]) << 8) | segment[2];
      geometry.width = (uint16_t(segment[3]) << 8) | segment[4];
      if (geometry.width == 0 || geometry.height == 0 ||
          geometry.width > max_width || geometry.height > max_height)
        return false;
      for (uint8_t i = 0; i < components; ++i) {
        ids[i] = segment[6 + 3 * i];
        const uint8_t sampling = segment[7 + 3 * i];
        if ((sampling >> 4) == 0 || (sampling >> 4) > 2 ||
            (sampling & 15) == 0 || (sampling & 15) > 2 ||
            segment[8 + 3 * i] > 3)
          return false;
        for (uint8_t j = 0; j < i; ++j)
          if (ids[j] == ids[i])
            return false;
      }
    } else if (marker == 0xda) {
      if (components == 0 || payload != 4U + 2U * components ||
          segment[0] != components)
        return false;
      for (uint8_t i = 0; i < components; ++i)
        if (segment[1 + 2 * i] != ids[i] || (segment[2 + 2 * i] >> 4) > 3 ||
            (segment[2 + 2 * i] & 15) > 3)
          return false;
      if (segment[payload - 3] != 0 || segment[payload - 2] != 63 ||
          segment[payload - 1] != 0)
        return false;
      offset += 2 + length;
      while (offset < size) {
        if (data[offset++] != 0xff)
          continue;
        if (offset == size)
          return false;
        const uint8_t entropy_marker = data[offset++];
        if (entropy_marker == 0xd9)
          return true;
        if (entropy_marker != 0 &&
            (entropy_marker < 0xd0 || entropy_marker > 0xd7))
          return false;
      }
      return false;
    } else if (marker == 0xdb) {
      size_t p = 0;
      while (p < payload) {
        if (segment[p] > 3 || payload - p < 65)
          return false; // 8-bit quantization tables only.
        p += 65;
      }
      if (p == 0)
        return false;
    } else if (marker == 0xc4) {
      size_t p = 0;
      while (p < payload) {
        if (payload - p < 17 || (segment[p] & 0xe0) != 0 ||
            (segment[p] & 15) > 3)
          return false;
        size_t symbols = 0;
        for (size_t i = 1; i <= 16; ++i)
          symbols += segment[p + i];
        if (symbols == 0 || symbols > 256 || symbols > payload - p - 17)
          return false;
        p += 17 + symbols;
      }
      if (p == 0)
        return false;
    } else if (marker == 0xdd) {
      if (payload != 2)
        return false;
    } else if ((marker < 0xe0 || marker > 0xef) && marker != 0xfe) {
      return false;
    }
    offset += 2 + length;
  }
  return false;
}

} // namespace esphome::esp_video_renderer
