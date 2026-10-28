#!/usr/bin/env python3
"""Check RGB565 pixels using the official esp_jpeg wrapper and software backend.

Requires Pillow, gcc/g++, and an unpacked espressif/esp_jpeg 1.3.2 component.
This host gate validates pixels and input bounds, not S3 ROM execution or timing.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/esp_video_renderer"


def qualify(component: Path, output: Path, input_jpeg: Path | None = None):
    output.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (64, 32))
    draw = ImageDraw.Draw(image)
    colors = [(240, 16, 16), (16, 240, 16), (16, 16, 240), (224, 224, 224)]
    for i, color in enumerate(colors):
        draw.rectangle((i * 16, 0, i * 16 + 15, 31), fill=color)
    cases = []
    for name, source_image, subsampling in (
        ("444", image, 0), ("422", image, 1), ("420", image, 2),
        ("grayscale", image.convert("L"), 0),
    ):
        for shift in range(3):
            case = f"{name}-scale{shift}"
            source_image.resize((64 << shift, 32 << shift)).save(
                output / f"input-{case}.jpg", quality=95, subsampling=subsampling
            )
            cases.append((case, 64, 32, shift))
    for name, size, shift in (("odd-half", (641, 479), 1), ("odd-quarter", (1279, 959), 2)):
        image.resize(size).save(output / f"input-{name}.jpg", quality=95, subsampling=2)
        cases.append((name, 320, 240, shift))
    if input_jpeg is not None:
        (output / "input-captured.jpg").write_bytes(input_jpeg.read_bytes())
        cases.append(("captured", 320, 240, None))
    decoded_info = {}
    with tempfile.TemporaryDirectory(prefix="esp-jpeg-pixels-") as temporary:
        build = Path(temporary)
        headers = {
            "sdkconfig.h": "#define CONFIG_JD_USE_ROM 0\n#define CONFIG_JD_SZBUF 512\n#define CONFIG_JD_FORMAT 0\n#define CONFIG_JD_USE_SCALE 1\n#define CONFIG_JD_TBLCLIP 1\n#define CONFIG_JD_FASTDECODE 0\n",
            "freertos/FreeRTOS.h": "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n#include <stdbool.h>\n#include <stdlib.h>\n#include <assert.h>\n#include \"sdkconfig.h\"\n",
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 257\n#define ESP_ERR_INVALID_ARG 258\n",
            "esp_system.h": "#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_DEFAULT 0\nstatic inline void*heap_caps_malloc(size_t n,unsigned c){(void)n;(void)c;abort();}\n",
            "esp_rom_caps.h": "#pragma once\n",
            "esp_log.h": "#pragma once\n#define ESP_LOGE(...) ((void)0)\n",
            "esp_check.h": "#pragma once\n#define ESP_GOTO_ON_FALSE(c,e,l,...) do{if(!(c)){ret=e;goto l;}}while(0)\n#define ESP_RETURN_ON_FALSE(c,e,...) do{if(!(c))return e;}while(0)\n",
        }
        for name, content in headers.items():
            path = build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        includes = ["-I", str(build), "-I", str(component / "include"), "-I", str(component / "tjpgd")]
        common = ["-g0", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        for source in ("jpeg_decoder.c", "tjpgd/tjpgd.c"):
            subprocess.run(["gcc", "-std=c11", "-Wno-incompatible-pointer-types", *common, *includes,
                            "-c", str(component / source), "-o", str(build / (Path(source).stem + ".o"))], check=True, capture_output=True, text=True)
        harness = build / "pixels.cpp"
        harness.write_text(r'''
#include <cstdint>
#include <cstddef>
#include "jpeg_decoder.h"
#include "jpeg_frame.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
using namespace esphome::esp_video_renderer;
int main(int argc,char**argv){
 assert(argc==5);std::ifstream file(argv[1],std::ios::binary);
 std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(file)),{});
 const unsigned width=std::strtoul(argv[3],nullptr,10),height=std::strtoul(argv[4],nullptr,10);
 JpegDecodePlan plan;assert(plan_baseline_jpeg(jpeg.data(),jpeg.size(),width,height,plan));
 const auto expected=plan.output;
 std::vector<uint8_t> pixels(width*height*2+32,0x5a);alignas(16) uint8_t workspace[3100];
 esp_jpeg_image_cfg_t config{};config.indata=jpeg.data();config.indata_size=jpeg.size();
 config.outbuf=pixels.data();config.outbuf_size=width*height*2;config.out_format=JPEG_IMAGE_FORMAT_RGB565;
 config.out_scale=static_cast<esp_jpeg_image_scale_t>(plan.scale);config.advanced.working_buffer=workspace;
 config.advanced.working_buffer_size=sizeof(workspace);esp_jpeg_image_output_t info{};
 assert(esp_jpeg_decode(&config,&info)==ESP_OK);assert(info.width==expected.width&&info.height==expected.height);
 assert(info.output_len==size_t(expected.width)*expected.height*2);
 for(size_t i=info.output_len;i<pixels.size();++i)assert(pixels[i]==0x5a);
 std::ofstream output(argv[2],std::ios::binary);output.write(reinterpret_cast<const char*>(pixels.data()),info.output_len);
 printf("{\"input_width\":%u,\"input_height\":%u,\"width\":%u,\"height\":%u,\"scale\":%u}",
        plan.input.width,plan.input.height,info.width,info.height,plan.scale);
 for(size_t length=0;length<jpeg.size();++length){
  std::vector<uint8_t> truncated(jpeg.begin(),jpeg.begin()+length);
  assert(!plan_baseline_jpeg(truncated.data(),length,width,height,plan));
 }
 for(size_t offset=2;offset+4<jpeg.size();){
  const uint8_t marker=jpeg[offset+1];const size_t length=(size_t(jpeg[offset+2])<<8)|jpeg[offset+3];
  auto malformed=jpeg;
  if(marker==0xc0){
   malformed[offset+1]=0xc2;assert(!plan_baseline_jpeg(malformed.data(),malformed.size(),width,height,plan));
   malformed=jpeg;malformed[offset+7]=255;malformed[offset+8]=255;
   assert(!plan_baseline_jpeg(malformed.data(),malformed.size(),width,height,plan));
  }
  if(marker==0xdb){malformed[offset+4]=0x10;assert(!plan_baseline_jpeg(malformed.data(),malformed.size(),width,height,plan));}
  if(marker==0xc4){malformed[offset+5]=255;assert(!plan_baseline_jpeg(malformed.data(),malformed.size(),width,height,plan));}
  if(marker==0xda){malformed[offset+length]=62;assert(!plan_baseline_jpeg(malformed.data(),malformed.size(),width,height,plan));break;}
  offset+=2+length;
 }
}
''')
        binary = build / "pixels"
        subprocess.run(["g++", "-std=c++17", *common, *includes, "-I", str(SOURCE), str(harness),
                        str(build / "jpeg_decoder.o"), str(build / "tjpgd.o"), "-o", str(binary)], check=True, capture_output=True, text=True)
        environment = os.environ | {"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        for name, width, height, shift in cases:
            decoded_run = subprocess.run([str(binary), str(output / f"input-{name}.jpg"), str(output / f"output-{name}.rgb565"), str(width), str(height)],
                                         check=True, capture_output=True, text=True, env=environment)
            decoded_info[name] = json.loads(decoded_run.stdout)
            if shift is not None:
                assert decoded_info[name]["scale"] == shift
            (output / f"sanitizer-{name}.log").write_text(decoded_run.stderr)
    results = []
    for name, _, _, _ in cases:
        raw = (output / f"output-{name}.rgb565").read_bytes()
        info = decoded_info[name]
        width, height = info["width"], info["height"]
        with Image.open(output / f"input-{name}.jpg") as source_image:
            # TJpgD drops the final partial row/column before block averaging.
            divisor = 1 << info["scale"]
            expected = source_image.convert("RGB").crop((0, 0, width * divisor, height * divisor))
            expected = expected.resize((width, height), Image.Resampling.BOX)
        decoded = Image.new("RGB", (width, height))
        errors = []
        for y in range(height):
            for x in range(width):
                pixel = int.from_bytes(raw[(y * width + x) * 2: (y * width + x) * 2 + 2], "little")
                rgb = ((pixel >> 11) * 255 // 31, ((pixel >> 5) & 63) * 255 // 63, (pixel & 31) * 255 // 31)
                decoded.putpixel((x, y), rgb)
                errors.extend(abs(a - b) for a, b in zip(rgb, expected.getpixel((x, y))))
        decoded.save(output / f"output-{name}.png")
        mean_error = sum(errors) / len(errors)
        assert mean_error < 8, (name, mean_error)
        results.append({"sampling": name, **info, "pixel_mean_absolute_error": mean_error, "output_bytes": len(raw),
                        "input_sha256": hashlib.sha256((output / f"input-{name}.jpg").read_bytes()).hexdigest()})
    result = {"passed": True, "component": "espressif/esp_jpeg@1.3.2", "source_sha256": hashlib.sha256((component / "jpeg_decoder.c").read_bytes()).hexdigest(),
              "workspace_bytes": 3100, "cases": results, "format": "RGB565_LE", "sanitizers": "ASan/UBSan",
              "scope": "official wrapper plus software TJpgD host backend, not ROM or hardware timing"}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--input-jpeg", type=Path, help="Also replay a captured real JPEG into a 320x240 surface")
    args = parser.parse_args()
    qualify(args.component.resolve(), args.output.resolve(), args.input_jpeg.resolve() if args.input_jpeg else None)


if __name__ == "__main__":
    main()
