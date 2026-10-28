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


def test_native_half_quarter_selection_and_input_work_bounds(tmp_path):
    harness = tmp_path / "plan.cpp"
    harness.write_text(r'''
#include "jpeg_frame.h"
#include <cassert>
#include <vector>
using namespace esphome::esp_video_renderer;
std::vector<uint8_t> frame(unsigned width,unsigned height) {
 std::vector<uint8_t> jpeg={0xff,0xd8,0xff,0xc0,0,11,8,0,8,0,8,1,1,0x11,0,
                           0xff,0xda,0,8,1,1,0,0,63,0,11,0xff,0xd9};
 jpeg[7]=height>>8;jpeg[8]=height;jpeg[9]=width>>8;jpeg[10]=width;return jpeg;
}
int main() {
 JpegDecodePlan plan;
 for(const auto &example:std::vector<std::vector<unsigned>>{
     {320,240,320,240,0,320,240},{640,360,320,240,1,320,180},
     {1280,720,320,240,2,320,180},{1280,960,320,240,2,320,240},
     {641,361,320,240,1,320,180},{1279,959,320,240,2,319,239},
     {720,1280,240,320,2,180,320},{8,8,320,240,0,8,8}}) {
  auto jpeg=frame(example[0],example[1]);
  assert(plan_baseline_jpeg(jpeg.data(),jpeg.size(),example[2],example[3],plan));
  assert(plan.input.width==example[0]&&plan.input.height==example[1]);
  assert(plan.scale==example[4]&&plan.output.width==example[5]&&plan.output.height==example[6]);
  assert(size_t(plan.output.width)*plan.output.height*2<=size_t(example[2])*example[3]*2);
 }
 for(const auto &dimensions:std::vector<std::pair<unsigned,unsigned>>{
     {1281,720},{1280,961},{1280,1280},{640,1},{0,240},{320,0},{65535,65535}}) {
  auto jpeg=frame(dimensions.first,dimensions.second);
  assert(!plan_baseline_jpeg(jpeg.data(),jpeg.size(),320,240,plan));
 }
 auto jpeg=frame(640,360);
 assert(!plan_baseline_jpeg(jpeg.data(),jpeg.size(),0,240,plan));
 assert(!plan_baseline_jpeg(jpeg.data(),jpeg.size(),320,0,plan));
 auto oversized=frame(1280,1000);
 assert(!plan_baseline_jpeg(oversized.data(),oversized.size(),640,480,plan));
 for(size_t length=0;length<jpeg.size();++length) {
  std::vector<uint8_t> short_frame(jpeg.begin(),jpeg.begin()+length);
  assert(!plan_baseline_jpeg(short_frame.data(),short_frame.size(),320,240,plan));
 }
 jpeg[3]=0xc2;assert(!plan_baseline_jpeg(jpeg.data(),jpeg.size(),320,240,plan));
}
''')
    binary = tmp_path / "plan"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(SOURCE), str(harness), "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
