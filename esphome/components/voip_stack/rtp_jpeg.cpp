#include "rtp_jpeg.h"

#ifdef USE_ESPHOME_VOIP_STACK_VIDEO

#include <algorithm>
#include <cstring>

namespace esphome {
namespace voip_stack {

namespace {

uint16_t read_be16(const uint8_t *data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                               data[1]);
}

void write_be16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value);
}

class BoundedWriter {
 public:
  BoundedWriter(uint8_t *data, size_t capacity)
      : data_(data), capacity_(capacity) {}

  bool append(const void *data, size_t size) {
    if (data == nullptr || this->size_ + size > this->capacity_) return false;
    memcpy(this->data_ + this->size_, data, size);
    this->size_ += size;
    return true;
  }
  bool byte(uint8_t value) { return this->append(&value, 1); }
  bool be16(uint16_t value) {
    uint8_t data[2];
    write_be16(data, value);
    return this->append(data, sizeof(data));
  }
  size_t size() const { return this->size_; }

 private:
  uint8_t *data_;
  size_t capacity_;
  size_t size_{0};
};

// Standard tables from RFC 2435 appendices A/B and JPEG Annex K. They live in
// flash and are copied only when reconstructing a received abbreviated frame.
static constexpr char JPEG_BASE_QUANTIZERS[] =
    "\x10\x0b\x0c\x0e\x0c\x0a\x10\x0e\x0d\x0e\x12\x11\x10\x13\x18\x28"
    "\x1a\x18\x16\x16\x18\x31\x23\x25\x1d\x28\x3a\x33\x3d\x3c\x39\x33"
    "\x38\x37\x40\x48\x5c\x4e\x40\x44\x57\x45\x37\x38\x50\x6d\x51\x57"
    "\x5f\x62\x67\x68\x67\x3e\x4d\x71\x79\x70\x64\x78\x5c\x65\x67\x63"
    "\x11\x12\x12\x18\x15\x18\x2f\x1a\x1a\x2f\x63\x42\x38\x42\x63\x63"
    "\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63"
    "\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63"
    "\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63\x63";
static_assert(sizeof(JPEG_BASE_QUANTIZERS) - 1 == 128);

static constexpr char JPEG_STANDARD_DHT[] =
    "\xff\xc4\x01\xa2\x00\x00\x01\x05\x01\x01\x01\x01\x01\x01\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a"
    "\x0b\x01\x00\x03\x01\x01\x01\x01\x01\x01\x01\x01\x01\x00\x00\x00"
    "\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x10\x00"
    "\x02\x01\x03\x03\x02\x04\x03\x05\x05\x04\x04\x00\x00\x01\x7d\x01"
    "\x02\x03\x00\x04\x11\x05\x12\x21\x31\x41\x06\x13\x51\x61\x07\x22"
    "\x71\x14\x32\x81\x91\xa1\x08\x23\x42\xb1\xc1\x15\x52\xd1\xf0\x24"
    "\x33\x62\x72\x82\x09\x0a\x16\x17\x18\x19\x1a\x25\x26\x27\x28\x29"
    "\x2a\x34\x35\x36\x37\x38\x39\x3a\x43\x44\x45\x46\x47\x48\x49\x4a"
    "\x53\x54\x55\x56\x57\x58\x59\x5a\x63\x64\x65\x66\x67\x68\x69\x6a"
    "\x73\x74\x75\x76\x77\x78\x79\x7a\x83\x84\x85\x86\x87\x88\x89\x8a"
    "\x92\x93\x94\x95\x96\x97\x98\x99\x9a\xa2\xa3\xa4\xa5\xa6\xa7\xa8"
    "\xa9\xaa\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xc2\xc3\xc4\xc5\xc6"
    "\xc7\xc8\xc9\xca\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xe1\xe2\xe3"
    "\xe4\xe5\xe6\xe7\xe8\xe9\xea\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9"
    "\xfa\x11\x00\x02\x01\x02\x04\x04\x03\x04\x07\x05\x04\x04\x00\x01"
    "\x02\x77\x00\x01\x02\x03\x11\x04\x05\x21\x31\x06\x12\x41\x51\x07"
    "\x61\x71\x13\x22\x32\x81\x08\x14\x42\x91\xa1\xb1\xc1\x09\x23\x33"
    "\x52\xf0\x15\x62\x72\xd1\x0a\x16\x24\x34\xe1\x25\xf1\x17\x18\x19"
    "\x1a\x26\x27\x28\x29\x2a\x35\x36\x37\x38\x39\x3a\x43\x44\x45\x46"
    "\x47\x48\x49\x4a\x53\x54\x55\x56\x57\x58\x59\x5a\x63\x64\x65\x66"
    "\x67\x68\x69\x6a\x73\x74\x75\x76\x77\x78\x79\x7a\x82\x83\x84\x85"
    "\x86\x87\x88\x89\x8a\x92\x93\x94\x95\x96\x97\x98\x99\x9a\xa2\xa3"
    "\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba"
    "\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xd2\xd3\xd4\xd5\xd6\xd7\xd8"
    "\xd9\xda\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xf2\xf3\xf4\xf5\xf6"
    "\xf7\xf8\xf9\xfa";
static_assert(sizeof(JPEG_STANDARD_DHT) - 1 == 420);

bool standard_huffman_table(uint8_t table_id, const uint8_t **table,
                            size_t *table_size) {
  if (table == nullptr || table_size == nullptr) return false;
  const auto *data =
      reinterpret_cast<const uint8_t *>(JPEG_STANDARD_DHT) + 4;
  const size_t size = sizeof(JPEG_STANDARD_DHT) - 1 - 4;
  size_t offset = 0;
  while (offset < size) {
    const size_t start = offset;
    if (offset + 17 > size) return false;
    const uint8_t current_id = data[offset++];
    size_t values = 0;
    for (size_t index = 0; index < 16; index++)
      values += data[offset + index];
    offset += 16;
    if (values == 0 || offset + values > size) return false;
    offset += values;
    if (current_id == table_id) {
      *table = data + start;
      *table_size = offset - start;
      return true;
    }
  }
  return false;
}

bool consume_standard_huffman_tables(const uint8_t *segment,
                                     size_t segment_size,
                                     uint8_t *table_mask) {
  if (segment == nullptr || table_mask == nullptr) return false;
  size_t offset = 0;
  while (offset < segment_size) {
    const size_t start = offset;
    if (offset + 17 > segment_size) return false;
    const uint8_t table_id = segment[offset++];
    uint8_t mask = 0;
    switch (table_id) {
      case 0x00:
        mask = 0x01;
        break;
      case 0x01:
        mask = 0x02;
        break;
      case 0x10:
        mask = 0x04;
        break;
      case 0x11:
        mask = 0x08;
        break;
      default:
        return false;
    }
    if ((*table_mask & mask) != 0) return false;
    size_t values = 0;
    for (size_t index = 0; index < 16; index++)
      values += segment[offset + index];
    offset += 16;
    if (values == 0 || offset + values > segment_size) return false;
    offset += values;
    const uint8_t *standard = nullptr;
    size_t standard_size = 0;
    if (!standard_huffman_table(table_id, &standard, &standard_size) ||
        standard_size != offset - start ||
        memcmp(standard, segment + start, standard_size) != 0) {
      return false;
    }
    *table_mask |= mask;
  }
  return true;
}

bool write_interchange_header(uint8_t type, uint8_t width_blocks,
                              uint8_t height_blocks,
                              const uint8_t *quantizers,
                              size_t quantizer_size,
                              uint16_t restart_interval, uint8_t *output,
                              size_t capacity, size_t *written) {
  if (type > 1 || width_blocks == 0 || height_blocks == 0 ||
      quantizers == nullptr || quantizer_size != 128 || output == nullptr ||
      written == nullptr) {
    return false;
  }
  BoundedWriter out(output, capacity);
  static constexpr char SOI_JFIF[] =
      "\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x02\x00\x00\x01\x00\x01\x00\x00";
  if (!out.append(SOI_JFIF, sizeof(SOI_JFIF) - 1) ||
      !out.append("\xff\xdb", 2) || !out.be16(132)) {
    return false;
  }
  for (uint8_t table = 0; table < 2; table++) {
    if (!out.byte(table) || !out.append(quantizers + table * 64, 64))
      return false;
  }
  if (restart_interval != 0 &&
      (!out.append("\xff\xdd\x00\x04", 4) ||
       !out.be16(restart_interval))) {
    return false;
  }
  if (!out.append("\xff\xc0\x00\x11\x08", 5) ||
      !out.be16(static_cast<uint16_t>(height_blocks) * 8) ||
      !out.be16(static_cast<uint16_t>(width_blocks) * 8)) {
    return false;
  }
  const uint8_t components[] = {
      3, 1, static_cast<uint8_t>(type == 1 ? 0x22 : 0x21), 0,
      2, 0x11, 1, 3, 0x11, 1,
  };
  static constexpr char SOS[] =
      "\xff\xda\x00\x0c\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00";
  if (!out.append(components, sizeof(components)) ||
      !out.append(JPEG_STANDARD_DHT, sizeof(JPEG_STANDARD_DHT) - 1) ||
      !out.append(SOS, sizeof(SOS) - 1)) {
    return false;
  }
  *written = out.size();
  return true;
}

void make_quantizers(uint8_t quality, uint8_t *output) {
  const int scale =
      quality < 50 ? 5000 / quality : 200 - static_cast<int>(quality) * 2;
  const auto *base =
      reinterpret_cast<const uint8_t *>(JPEG_BASE_QUANTIZERS);
  for (size_t index = 0; index < 128; index++) {
    output[index] = static_cast<uint8_t>(
        std::clamp((static_cast<int>(base[index]) * scale + 50) / 100,
                   1, 255));
  }
}

bool valid_quantizers(const uint8_t *quantizers, size_t size) {
  if (quantizers == nullptr || size != 128) return false;
  for (size_t index = 0; index < size; index++) {
    // JPEG quantization values are unsigned integers in the range 1..255.
    // Letting a zero reach the hardware decoder is both invalid and unsafe.
    if (quantizers[index] == 0) return false;
  }
  return true;
}

}  // namespace

bool parse_jpeg_for_rtp(const uint8_t *data, size_t size,
                        RtpJpegFrameView *frame) {
  if (data == nullptr || frame == nullptr || size < 4 || data[0] != 0xFF ||
      data[1] != 0xD8) {
    return false;
  }
  *frame = {};
  bool quantizer_present[2]{};
  bool saw_sof = false;
  bool saw_dht = false;
  uint8_t huffman_table_mask = 0;
  size_t position = 2;
  while (position + 1 < size) {
    if (data[position] != 0xFF) return false;
    while (position < size && data[position] == 0xFF) position++;
    if (position >= size) return false;
    const uint8_t marker = data[position++];
    if (marker == 0xD9) break;
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) ||
        marker == 0x01) {
      continue;
    }
    if (position + 2 > size) return false;
    const uint16_t segment_length = read_be16(data + position);
    if (segment_length < 2 ||
        position + static_cast<size_t>(segment_length) > size) {
      return false;
    }
    const uint8_t *segment = data + position + 2;
    const size_t payload_size = segment_length - 2;
    if (marker == 0xDB) {
      size_t offset = 0;
      while (offset < payload_size) {
        const uint8_t descriptor = segment[offset++];
        const uint8_t precision = descriptor >> 4;
        const uint8_t table = descriptor & 0x0F;
        if (precision != 0 || table > 1 || quantizer_present[table] ||
            offset + 64 > payload_size)
          return false;
        memcpy(frame->quantizers.data() + table * 64, segment + offset, 64);
        quantizer_present[table] = true;
        offset += 64;
      }
    } else if (marker == 0xC4) {
      if (!consume_standard_huffman_tables(
              segment, payload_size, &huffman_table_mask)) {
        return false;
      }
      saw_dht = true;
    } else if (marker == 0xC0) {
      if (payload_size != 15 || segment[0] != 8 || segment[5] != 3)
        return false;
      frame->height = read_be16(segment + 1);
      frame->width = read_be16(segment + 3);
      if (frame->width == 0 || frame->height == 0 || frame->width > 2040 ||
          frame->height > 2040) {
        return false;
      }
      const uint8_t *components = segment + 6;
      if (components[0] != 1 || components[2] != 0 ||
          components[3] != 2 || components[4] != 0x11 ||
          components[5] != 1 || components[6] != 3 ||
          components[7] != 0x11 || components[8] != 1) {
        return false;
      }
      if (components[1] == 0x21)
        frame->type = 0;
      else if (components[1] == 0x22)
        frame->type = 1;
      else
        return false;
      saw_sof = true;
    } else if (marker == 0xDD) {
      if (payload_size != 2) return false;
      frame->restart_interval = read_be16(segment);
      if (frame->restart_interval == 0) return false;
    } else if (marker == 0xDA) {
      if (!saw_sof || !quantizer_present[0] || !quantizer_present[1] ||
          (saw_dht && huffman_table_mask != 0x0F) ||
          payload_size != 10 || segment[0] != 3 ||
          memcmp(segment + 1, "\x01\x00\x02\x11\x03\x11\x00\x3f\x00",
                 9) != 0) {
        return false;
      }
      const size_t scan_start = position + segment_length;
      size_t scan_end = size;
      bool found_eoi = false;
      while (scan_end >= scan_start + 2) {
        if (data[scan_end - 2] == 0xFF && data[scan_end - 1] == 0xD9) {
          scan_end -= 2;
          found_eoi = true;
          break;
        }
        scan_end--;
      }
      if (!found_eoi || scan_end <= scan_start) return false;
      frame->scan = data + scan_start;
      frame->scan_size = scan_end - scan_start;
      return true;
    } else if (marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 &&
               marker != 0xC8 && marker != 0xCC) {
      // Progressive, lossless and hierarchical SOF markers cannot be mapped
      // to RFC 2435's fixed baseline types.
      return false;
    }
    position += segment_length;
  }
  return false;
}

size_t build_rtp_jpeg_fragment_header(const RtpJpegFrameView &frame,
                                      uint32_t fragment_offset,
                                      uint8_t *output, size_t capacity) {
  const bool first = fragment_offset == 0;
  const size_t required =
      8 + (frame.restart_interval != 0 ? 4 : 0) + (first ? 132 : 0);
  if (output == nullptr || capacity < required || frame.scan == nullptr ||
      frame.scan_size == 0 || frame.type > 1 || frame.width == 0 ||
      frame.height == 0 || frame.width > 2040 || frame.height > 2040 ||
      fragment_offset > 0xFFFFFF) {
    return 0;
  }
  output[0] = 0;
  output[1] = static_cast<uint8_t>(fragment_offset >> 16);
  output[2] = static_cast<uint8_t>(fragment_offset >> 8);
  output[3] = static_cast<uint8_t>(fragment_offset);
  output[4] =
      static_cast<uint8_t>(frame.type | (frame.restart_interval ? 0x40 : 0));
  output[5] = 255;
  output[6] = static_cast<uint8_t>((frame.width + 7) / 8);
  output[7] = static_cast<uint8_t>((frame.height + 7) / 8);
  size_t cursor = 8;
  if (frame.restart_interval != 0) {
    write_be16(output + cursor, frame.restart_interval);
    // RFC 2435 section 3.1.7 fallback: packet boundaries are not aligned to
    // restart intervals, so the receiver reassembles the complete frame.
    write_be16(output + cursor + 2, 0xFFFF);
    cursor += 4;
  }
  if (first) {
    output[cursor] = 0;
    output[cursor + 1] = 0;
    write_be16(output + cursor + 2, 128);
    memcpy(output + cursor + 4, frame.quantizers.data(), 128);
    cursor += 132;
  }
  return cursor;
}

bool RtpJpegDepacketizer::start_frame_(
    uint8_t type_specific, uint8_t type, uint8_t quality,
    uint8_t width_blocks, uint8_t height_blocks, uint16_t restart_interval,
    const uint8_t *quantizers, size_t quantizer_size, uint32_t timestamp,
    uint8_t *output, size_t output_capacity) {
  if (type_specific != 0 || type > 1 || width_blocks == 0 ||
      height_blocks == 0 || quantizers == nullptr || quantizer_size != 128) {
    return false;
  }
  size_t header_size = 0;
  if (!write_interchange_header(type, width_blocks, height_blocks, quantizers,
                                quantizer_size, restart_interval, output,
                                output_capacity, &header_size)) {
    return false;
  }
  this->active_ = true;
  this->timestamp_ = timestamp;
  this->type_specific_ = type_specific;
  this->type_ = static_cast<uint8_t>(
      type | (restart_interval != 0 ? 0x40 : 0));
  this->quality_ = quality;
  this->width_blocks_ = width_blocks;
  this->height_blocks_ = height_blocks;
  this->restart_interval_ = restart_interval;
  this->header_size_ = header_size;
  this->scan_size_ = 0;
  return true;
}

void RtpJpegDepacketizer::set_quantization_cache(uint8_t *storage,
                                                 size_t capacity) {
  uint8_t *const usable =
      storage != nullptr && capacity >= kQuantizationCacheBytes ? storage
                                                                : nullptr;
  if (usable == this->quantization_cache_) return;
  this->quantization_cache_ = usable;
  this->reset_session();
}

const uint8_t *RtpJpegDepacketizer::find_cached_quantizers_(
    uint8_t quality) const {
  if (this->quantization_cache_ == nullptr ||
      quality < kFirstCachedQuality || quality > kLastCachedQuality) {
    return nullptr;
  }
  const size_t index = quality - kFirstCachedQuality;
  if ((this->cached_quantizers_valid_[index / 8] &
       static_cast<uint8_t>(1U << (index % 8))) == 0) {
    return nullptr;
  }
  return this->quantization_cache_ + index * kQuantizationTableBytes;
}

bool RtpJpegDepacketizer::cache_or_validate_quantizers_(
    uint8_t quality, const uint8_t *quantizers) {
  if (this->quantization_cache_ == nullptr || quantizers == nullptr ||
      quality < kFirstCachedQuality || quality > kLastCachedQuality) {
    return false;
  }
  const size_t index = quality - kFirstCachedQuality;
  const uint8_t bit = static_cast<uint8_t>(1U << (index % 8));
  uint8_t &valid = this->cached_quantizers_valid_[index / 8];
  uint8_t *const cached =
      this->quantization_cache_ + index * kQuantizationTableBytes;
  if ((valid & bit) != 0) {
    // RFC 2435 section 3.1.8 requires a Q value to keep the same mapping for
    // the complete RTP session.
    return memcmp(cached, quantizers, kQuantizationTableBytes) == 0;
  }
  memcpy(cached, quantizers, kQuantizationTableBytes);
  valid |= bit;
  return true;
}

RtpJpegPushResult RtpJpegDepacketizer::push(
    const uint8_t *payload, size_t payload_size, bool marker,
    uint32_t timestamp, uint8_t *output, size_t output_capacity,
    size_t *output_size) {
  if (output_size != nullptr) *output_size = 0;
  if (payload == nullptr || payload_size < 8 || output == nullptr ||
      output_size == nullptr) {
    this->reset();
    return RtpJpegPushResult::DROPPED;
  }
  const uint8_t type_specific = payload[0];
  const uint32_t fragment_offset =
      (static_cast<uint32_t>(payload[1]) << 16) |
      (static_cast<uint32_t>(payload[2]) << 8) | payload[3];
  const uint8_t type_raw = payload[4];
  const uint8_t quality = payload[5];
  const uint8_t width_blocks = payload[6];
  const uint8_t height_blocks = payload[7];
  uint8_t type = type_raw;
  size_t cursor = 8;
  uint16_t restart_interval = 0;
  if ((type & 0x40) != 0) {
    if (payload_size < cursor + 4) {
      this->reset();
      return RtpJpegPushResult::DROPPED;
    }
    restart_interval = read_be16(payload + cursor);
    cursor += 4;
    type &= ~0x40;
    if (restart_interval == 0) {
      this->reset();
      return RtpJpegPushResult::DROPPED;
    }
  }
  if (type > 1 || type_specific != 0 || width_blocks == 0 ||
      height_blocks == 0) {
    this->reset();
    return RtpJpegPushResult::DROPPED;
  }

  if (fragment_offset == 0) {
    uint8_t generated_quantizers[128];
    uint8_t expanded_quantizers[128];
    const uint8_t *quantizers = nullptr;
    size_t quantizer_size = 0;
    if (quality >= 128) {
      if (payload_size < cursor + 4 || payload[cursor] != 0 ||
          payload[cursor + 1] != 0) {
        this->reset();
        return RtpJpegPushResult::DROPPED;
      }
      quantizer_size = read_be16(payload + cursor + 2);
      cursor += 4;
      if (quantizer_size == 0) {
        quantizers = this->find_cached_quantizers_(quality);
        if (quality == 255 || quantizers == nullptr) {
          this->reset();
          return RtpJpegPushResult::DROPPED;
        }
        quantizer_size = kQuantizationTableBytes;
      } else {
        const size_t wire_quantizer_size = quantizer_size;
        if ((wire_quantizer_size != 64 &&
             wire_quantizer_size != kQuantizationTableBytes) ||
            payload_size < cursor + quantizer_size) {
          this->reset();
          return RtpJpegPushResult::DROPPED;
        }
        if (wire_quantizer_size == 64) {
          // FFmpeg and deployed softphones emit one 8-bit table despite RFC
          // 2435 type 0/1 specifying luma + chroma. Accept that established
          // receiver-side extension by using the table for both components.
          // The packetizer remains strict and always transmits 128 bytes.
          memcpy(expanded_quantizers, payload + cursor, 64);
          memcpy(expanded_quantizers + 64, payload + cursor, 64);
          quantizers = expanded_quantizers;
          quantizer_size = sizeof(expanded_quantizers);
        } else {
          quantizers = payload + cursor;
        }
        cursor += wire_quantizer_size;
        if (!valid_quantizers(quantizers, quantizer_size) ||
            (quality != 255 &&
             !this->cache_or_validate_quantizers_(quality, quantizers))) {
          this->reset();
          return RtpJpegPushResult::DROPPED;
        }
      }
    } else {
      if (quality == 0 || quality > 99) {
        this->reset();
        return RtpJpegPushResult::DROPPED;
      }
      make_quantizers(quality, generated_quantizers);
      quantizers = generated_quantizers;
      quantizer_size = sizeof(generated_quantizers);
    }
    this->reset();
    if (!this->start_frame_(type_specific, type, quality, width_blocks,
                            height_blocks, restart_interval, quantizers,
                            quantizer_size, timestamp, output,
                            output_capacity)) {
      return RtpJpegPushResult::DROPPED;
    }
  } else if (!this->active_ || this->timestamp_ != timestamp ||
             this->type_specific_ != type_specific ||
             this->type_ != type_raw || this->quality_ != quality ||
             this->width_blocks_ != width_blocks ||
             this->height_blocks_ != height_blocks ||
             this->restart_interval_ != restart_interval ||
             fragment_offset != this->scan_size_) {
    this->reset();
    return RtpJpegPushResult::DROPPED;
  }

  const size_t fragment_size = payload_size - cursor;
  if (!this->active_ || fragment_offset != this->scan_size_ ||
      fragment_size == 0 ||
      this->header_size_ + this->scan_size_ + fragment_size + 2 >
          output_capacity) {
    this->reset();
    return RtpJpegPushResult::DROPPED;
  }
  memcpy(output + this->header_size_ + this->scan_size_, payload + cursor,
         fragment_size);
  this->scan_size_ += fragment_size;
  if (!marker) return RtpJpegPushResult::INCOMPLETE;

  output[this->header_size_ + this->scan_size_] = 0xFF;
  output[this->header_size_ + this->scan_size_ + 1] = 0xD9;
  *output_size = this->header_size_ + this->scan_size_ + 2;
  this->reset();
  return RtpJpegPushResult::COMPLETE;
}

void RtpJpegDepacketizer::reset() {
  this->active_ = false;
  this->timestamp_ = 0;
  this->type_specific_ = 0;
  this->type_ = 0;
  this->quality_ = 0;
  this->width_blocks_ = 0;
  this->height_blocks_ = 0;
  this->restart_interval_ = 0;
  this->header_size_ = 0;
  this->scan_size_ = 0;
}

void RtpJpegDepacketizer::reset_session() {
  this->reset();
  this->cached_quantizers_valid_.fill(0);
}

}  // namespace voip_stack
}  // namespace esphome

#endif
