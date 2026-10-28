"""Bounded JPEG admission rejects malformed input before vendor decoding."""

from pathlib import Path
import subprocess

SOURCE = Path(__file__).resolve().parents[1] / "esphome/components/esp_video_renderer"


def test_baseline_headers_entropy_and_geometry_are_bounded(tmp_path):
    harness = tmp_path / "jpeg.cpp"
    harness.write_text(r'''
#include "jpeg_frame.h"
#include <cassert>
#include <vector>
using namespace esphome::esp_video_renderer;
int main() {
  const std::vector<uint8_t> valid={0xff,0xd8,0xff,0xc0,0,11,8,0,8,0,8,1,1,0x11,0,
                                  0xff,0xda,0,8,1,1,0,0,63,0,11,0xff,0xd9};
  JpegGeometry geometry;
  assert(inspect_baseline_jpeg(valid.data(),valid.size(),8,8,geometry));
  assert(geometry.width==8&&geometry.height==8);
  for(size_t size=0;size<valid.size();++size) {
    // Exact-sized copies give ASan an independent truncated-input oracle.
    std::vector<uint8_t> truncated(valid.begin(),valid.begin()+size);
    assert(!inspect_baseline_jpeg(truncated.data(),truncated.size(),8,8,geometry));
  }
  for(auto mutation:std::vector<std::pair<size_t,uint8_t>>{{3,0xc2},{5,2},{6,12},{8,0},{10,16},
      {11,4},{13,0},{18,2},{19,3},{20,2},{23,62},{24,1},{27,0}}) {
    auto malformed=valid;malformed[mutation.first]=mutation.second;
    assert(!inspect_baseline_jpeg(malformed.data(),malformed.size(),8,8,geometry));
  }
  auto app=valid;app.insert(app.begin()+2,{0xff,0xe0,0,6,1,2,3,4});
  assert(inspect_baseline_jpeg(app.data(),app.size(),8,8,geometry));
  app[5]=255;assert(!inspect_baseline_jpeg(app.data(),app.size(),8,8,geometry));
  auto stuffed=valid;stuffed.insert(stuffed.begin()+26,{0xff,0,0xff,0xd0});
  assert(inspect_baseline_jpeg(stuffed.data(),stuffed.size(),8,8,geometry));
  // An incomplete DHT and DQT may not reach the vendor's table parser.
  for(auto segment:std::vector<std::vector<uint8_t>>{{0xff,0xc4,0,4,0,1},{0xff,0xdb,0,3,0}}) {
    auto malformed=valid;malformed.insert(malformed.begin()+2,segment.begin(),segment.end());
    assert(!inspect_baseline_jpeg(malformed.data(),malformed.size(),8,8,geometry));
  }
}
''')
    binary = tmp_path / "jpeg"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(SOURCE), str(harness), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
