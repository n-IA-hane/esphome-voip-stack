// Host-only Espressif API stand-in backed by the installed SpanDSP library.
// The production wrapper and exact Espressif headers are under test; the
// target-specific Espressif binary is not executable on this host.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "esp_g722_enc.h"
#include "esp_g722_dec.h"
#include "g722_rtp_codec.h"
// SpanDSP's legacy SAMPLE_RATE macro must not rewrite the Native type header.
#include <spandsp.h>

using namespace esphome::voip_stack;
namespace {
struct Encoder { g722_encode_state_t *state; };
struct Decoder { g722_decode_state_t *state; };
std::set<void *> live;
int failure = 0;
int encoder_opens = 0;
int decoder_opens = 0;
int encoder_resets = 0;
int decoder_resets = 0;
bool bad_size = false;
bool encode_failure = false;
bool encode_short = false;
bool decode_failure = false;
bool decode_partial = false;
bool decode_bad_info = false;
AudioFormat format(uint16_t ms) {
  AudioFormat value{16000, PcmFormat::S16LE, 1, ms, AudioCodec::G722};
  return value;
}
}

extern "C" {
esp_audio_err_t esp_g722_enc_open(void *value, uint32_t size, void **handle) {
  *handle = nullptr;
  assert(size == sizeof(esp_g722_enc_config_t));
  const auto &cfg = *static_cast<esp_g722_enc_config_t *>(value);
  assert(cfg.sample_rate == 16000 && cfg.channel == 1 && cfg.bits_per_sample == 16);
  assert(cfg.bitrate == 64000 && !cfg.packed && cfg.frame_duration == 10);
  if (failure == 1) return ESP_AUDIO_ERR_MEM_LACK;
  auto *encoder = new Encoder{g722_encode_init(nullptr, 64000, 0)};
  assert(encoder->state != nullptr);
  *handle = encoder;
  assert(live.insert(encoder).second);
  encoder_opens++;
  return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_g722_enc_get_frame_size(void *handle, int *input, int *output) {
  assert(live.count(handle) == 1);
  if (failure == 2) return ESP_AUDIO_ERR_FAIL;
  *input = bad_size ? 640 : 320;
  *output = 80;
  return ESP_AUDIO_ERR_OK;
}
void esp_g722_enc_close(void *handle) {
  assert(live.erase(handle) == 1);
  auto *encoder = static_cast<Encoder *>(handle);
  assert(g722_encode_free(encoder->state) == 0);
  delete encoder;
}
esp_audio_err_t esp_g722_enc_process(void *handle, esp_audio_enc_in_frame_t *input,
                                     esp_audio_enc_out_frame_t *output) {
  assert(live.count(handle) == 1);
  if (encode_failure) return ESP_AUDIO_ERR_FAIL;
  assert(input->len % 320 == 0 && output->len >= input->len / 4);
  const int bytes = g722_encode(static_cast<Encoder *>(handle)->state, output->buffer,
                               reinterpret_cast<const int16_t *>(input->buffer), input->len / 2);
  output->encoded_bytes = encode_short ? bytes - 1 : bytes;
  return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_g722_enc_reset(void *handle) {
  assert(live.count(handle) == 1);
  auto *encoder = static_cast<Encoder *>(handle);
  assert(g722_encode_init(encoder->state, 64000, 0) == encoder->state);
  encoder_resets++;
  return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_g722_dec_open(void *value, uint32_t size, void **handle) {
  *handle = nullptr;
  assert(size == sizeof(esp_g722_dec_cfg_t));
  const auto &cfg = *static_cast<esp_g722_dec_cfg_t *>(value);
  assert(cfg.sample_rate == 16000 && cfg.bitrate == 64000 && !cfg.packed);
  if (failure == 3) return ESP_AUDIO_ERR_MEM_LACK;
  auto *decoder = new Decoder{g722_decode_init(nullptr, 64000, 0)};
  assert(decoder->state != nullptr);
  *handle = decoder;
  assert(live.insert(decoder).second);
  decoder_opens++;
  return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_g722_dec_close(void *handle) {
  assert(live.erase(handle) == 1);
  auto *decoder = static_cast<Decoder *>(handle);
  assert(g722_decode_free(decoder->state) == 0);
  delete decoder;
  return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_g722_dec_decode(void *handle, esp_audio_dec_in_raw_t *input,
                                    esp_audio_dec_out_frame_t *output, esp_audio_dec_info_t *info) {
  assert(live.count(handle) == 1);
  if (decode_failure) return ESP_AUDIO_ERR_FAIL;
  assert(output->len >= input->len * 4);
  const int samples = g722_decode(static_cast<Decoder *>(handle)->state,
                                 reinterpret_cast<int16_t *>(output->buffer), input->buffer, input->len);
  output->decoded_size = samples * 2;
  input->consumed = decode_partial ? input->len - 1 : input->len;
  info->sample_rate = decode_bad_info ? 8000 : 16000;
  info->bits_per_sample = 16;
  info->channel = 1;
  return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_g722_dec_reset(void *handle) {
  assert(live.count(handle) == 1);
  auto *decoder = static_cast<Decoder *>(handle);
  assert(g722_decode_init(decoder->state, 64000, 0) == decoder->state);
  decoder_resets++;
  return ESP_AUDIO_ERR_OK;
}
}

static void contracts() {
  auto ten = format(10);
  auto twenty = format(20);
  auto pcm = ten;
  pcm.codec = AudioCodec::PCM;
  assert(audio_format_rtp_clock_rate(ten) == 8000);
  assert(ten.nominal_frame_samples() == 160 && ten.nominal_rtp_payload_bytes() == 80);
  assert(twenty.nominal_frame_samples() == 320 && twenty.nominal_rtp_payload_bytes() == 160);
  for (int at = 1; at <= 3; ++at) {
    G722RtpCodec codec;
    failure = at;
    assert(!codec.configure(&twenty, &twenty, 160));
    assert(live.empty() && !codec.ready_for(twenty, twenty));
    failure = 0;
    assert(codec.configure(&twenty, &twenty, 160));
    assert(live.size() == 2 && codec.ready_for(ten, twenty));
  }
  assert(live.empty());
  {
    G722RtpCodec codec;
    bad_size = true;
    assert(!codec.configure(&ten, &ten, 160) && live.empty());
    bad_size = false;
    auto invalid = ten;
    invalid.sample_rate = 8000;
    assert(!codec.configure(&invalid, nullptr, 160) && live.empty());
    invalid = ten;
    invalid.channels = 2;
    assert(!codec.configure(nullptr, &invalid, 160) && live.empty());
    invalid = ten;
    invalid.pcm_format = PcmFormat::S24LE;
    assert(!codec.configure(&invalid, nullptr, 160) && live.empty());
    invalid = ten;
    invalid.frame_ms = 16;
    assert(!codec.configure(&invalid, nullptr, 160) && live.empty());
    assert(!codec.configure(&twenty, &twenty, 159) && live.empty());
    assert(codec.configure(&ten, nullptr, 80) && live.size() == 1);
    assert(codec.ready_for(ten, pcm) && !codec.ready_for(twenty, ten));
    assert(codec.configure(nullptr, &ten, 80) && live.size() == 1);
    assert(codec.ready_for(pcm, ten) && !codec.ready_for(ten, ten));
    assert(codec.configure(nullptr, nullptr, 0) && live.empty());
  }
  {
    G722RtpCodec codec;
    assert(codec.configure(&twenty, &twenty, 160));
    const int opens = encoder_opens + decoder_opens;
    alignas(int16_t) uint8_t input[640]{};
    alignas(int16_t) uint8_t output[640]{};
    uint8_t payload[160]{};
    for (const auto &f : {ten, twenty, ten, twenty}) {
      const auto bytes = f.nominal_frame_bytes();
      const auto wire = f.nominal_rtp_payload_bytes();
      assert(codec.encode(input, bytes, f, payload, sizeof(payload)) == wire);
      assert(codec.decode(payload, wire, f, output, sizeof(output)) == bytes);
    }
    assert(encoder_opens + decoder_opens == opens && live.size() == 2);
    assert(codec.encode(nullptr, 640, twenty, payload, sizeof(payload)) == 0);
    assert(codec.encode(input, 639, twenty, payload, sizeof(payload)) == 0);
    assert(codec.encode(input, 640, twenty, payload, 159) == 0);
    assert(codec.decode(nullptr, 160, twenty, output, sizeof(output)) == 0);
    assert(codec.decode(payload, 159, twenty, output, sizeof(output)) == 0);
    assert(codec.decode(payload, 160, twenty, output, 639) == 0);
    assert(codec.decode(nullptr, 0, twenty, output, sizeof(output)) == 0);
    encode_failure = true;
    assert(codec.encode(input, 640, twenty, payload, sizeof(payload)) == 0);
    encode_failure = false;
    encode_short = true;
    assert(codec.encode(input, 640, twenty, payload, sizeof(payload)) == 0);
    encode_short = false;
    decode_failure = true;
    assert(codec.decode(payload, 160, twenty, output, sizeof(output)) == 0);
    decode_failure = false;
    decode_partial = true;
    assert(codec.decode(payload, 160, twenty, output, sizeof(output)) == 0);
    decode_partial = false;
    decode_bad_info = true;
    assert(codec.decode(payload, 160, twenty, output, sizeof(output)) == 0);
    decode_bad_info = false;
    codec.reset();
    assert(encoder_resets == 1 && decoder_resets == 1);
    codec.reset_decoder();
    assert(encoder_resets == 1 && decoder_resets == 2);
    assert(encoder_opens + decoder_opens == opens);
    assert(codec.encode(input, 640, twenty, payload, sizeof(payload)) == 160);
    assert(codec.decode(payload, 160, twenty, output, sizeof(output)) == 640);
  }
  assert(live.empty());
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "contract") {
    contracts();
    return 0;
  }
  assert(argc == 5);
  const bool encode = std::string(argv[1]) == "encode";
  const auto f = format(static_cast<uint16_t>(std::stoi(argv[2])));
  std::ifstream input(argv[3], std::ios::binary);
  assert(input.good());
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
  std::ofstream output(argv[4], std::ios::binary);
  assert(output.good());
  G722RtpCodec codec;
  assert(codec.configure(encode ? &f : nullptr, encode ? nullptr : &f, 160));
  const auto step = encode ? f.nominal_frame_bytes() : f.nominal_rtp_payload_bytes();
  assert(!data.empty() && data.size() % step == 0);
  alignas(int16_t) uint8_t result[640]{};
  for (size_t pos = 0; pos < data.size(); pos += step) {
    const size_t count = encode
        ? codec.encode(data.data() + pos, step, f, result, sizeof(result))
        : codec.decode(data.data() + pos, step, f, result, sizeof(result));
    assert(count == (encode ? f.nominal_rtp_payload_bytes() : f.nominal_frame_bytes()));
    output.write(reinterpret_cast<const char *>(result), count);
  }
}
