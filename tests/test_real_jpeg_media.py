"""Real UDP/RFC2435 media decoded by the pinned Espressif portable decoder.

Set ESP_JPEG_SOURCE to esp_jpeg 1.3.2 and ESP_VIDEO_RENDERER_SOURCE to the
standalone renderer checkout. No SDK, ROM, peripheral or acoustic claim.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    begin = source.index(signature)
    brace = source.index("{", begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


@pytest.fixture(scope="module", params=[0, 1, 2], ids=["basic", "barrel", "table"])
def real_jpeg_binary(tmp_path_factory, request):
    vendor = os.environ.get("ESP_JPEG_SOURCE")
    renderer = os.environ.get("ESP_VIDEO_RENDERER_SOURCE")
    if not vendor or not renderer:
        pytest.skip("Real codec gate requires ESP_JPEG_SOURCE and ESP_VIDEO_RENDERER_SOURCE")
    vendor = Path(vendor).resolve()
    renderer = Path(renderer).resolve() / "esphome/components/esp_video_renderer"
    assert "version: 1.3.2" in (vendor / "idf_component.yml").read_text()
    temporary = tmp_path_factory.mktemp("real-jpeg")
    headers = {
        "freertos/FreeRTOS.h": "#pragma once\n#include <stdint.h>\n#include <stdlib.h>\n#include <stdbool.h>\n#include <assert.h>\n",
        "esp_system.h": "#pragma once\n#define MALLOC_CAP_DEFAULT 0\n#define heap_caps_malloc(n,c) malloc(n)\n",
        "esp_rom_caps.h": "#pragma once\n",
        "esp_log.h": "#pragma once\n#define ESP_LOGE(...) ((void)0)\n",
        "esp_err.h": "#pragma once\n#include <stdint.h>\n#include <stddef.h>\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n#define ESP_ERR_INVALID_ARG 0x102\n",
        "esp_check.h": "#pragma once\n#include <stdio.h>\n#define ESP_GOTO_ON_FALSE(a,e,l,t,f,...) do{if(!(a)){fprintf(stderr,f \"\\n\",##__VA_ARGS__);ret=(e);goto l;}}while(0)\n#define ESP_RETURN_ON_FALSE(a,e,t,f,...) do{if(!(a)){fprintf(stderr,f \"\\n\",##__VA_ARGS__);return(e);}}while(0)\n",
        "sdkconfig.h": "#pragma once\n#define CONFIG_JD_USE_ROM 0\n#define CONFIG_JD_SZBUF 512\n#define CONFIG_JD_FORMAT 0\n#define CONFIG_JD_USE_SCALE 1\n#define CONFIG_JD_TBLCLIP 1\n#define CONFIG_JD_FASTDECODE " + str(request.param) + "\n#define CONFIG_JD_DEFAULT_HUFFMAN 0\n",
        "esphome/core/defines.h": "#pragma once\n",
    }
    for name, body in headers.items():
        path = temporary / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
    common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-g",
              "-I", str(temporary), "-I", str(vendor / "include"), "-I", str(vendor / "tjpgd")]
    objects = []
    for name in ["jpeg_decoder.c", "tjpgd/tjpgd.c"]:
        source = vendor / name
        if name == "jpeg_decoder.c":
            # ESP32 size_t is unsigned int. Only the callback ABI differs on
            # this 64-bit host; keep the vendor tree and decoder unchanged.
            original = source.read_text()
            callback = "static unsigned int jpeg_decode_in_cb(JDEC *"
            assert original.count(callback) == 2
            adapted = original.replace(callback, "static size_t jpeg_decode_in_cb(JDEC *")
            adapted = adapted.replace("uint8_t *buff, unsigned int nbyte)", "uint8_t *buff, size_t nbyte)")
            source = temporary / name
            source.write_text(adapted)
            (temporary / "host-abi.json").write_text(json.dumps({
                "original_sha256": hashlib.sha256(original.encode()).hexdigest(),
                "host_sha256": hashlib.sha256(adapted.encode()).hexdigest(),
                "adapter": "jpeg_decode_in_cb declaration and definition use size_t"}, indent=2) + "\n")
        output = temporary / (Path(name).stem + ".o")
        result = subprocess.run(["gcc", "-std=c99", *common, "-c", str(source), "-o", str(output)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    implementation = (renderer / "esp_video_renderer.cpp").read_text()
    workspace = implementation[implementation.index("#if !CONFIG_JD_USE_ROM"):implementation.index("static void update_max(")]
    decode = method(implementation, "bool EspVideoRenderer::decode_(")
    update = method(implementation, "static void update_max(")
    cpp = temporary / "media.cpp"
    cpp.write_text(r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "jpeg_decoder.h"
#include "jpeg_frame.h"
#include "sdkconfig.h"
#include "esphome/components/voip_stack/rtp_jpeg.h"
using namespace esphome::voip_stack;
namespace esphome::esp_video_renderer {
@WORKSPACE@
uint32_t micros(){return std::chrono::duration_cast<std::chrono::microseconds>(
 std::chrono::steady_clock::now().time_since_epoch()).count();}
@UPDATE@
struct EspVideoRenderer {
 struct Input{uint8_t*data;size_t size;};struct Surface{uint8_t*data;uint16_t width=0,height=0;};
 Input inputs_[1];Surface surfaces_[1];uint16_t width_,height_;size_t surface_capacity_;
 alignas(16) uint8_t decoder_workspace_[JPEG_WORKSPACE_BYTES];
 std::atomic<uint32_t>decode_errors_{0},decoded_{0},decode_max_us_{0};
 bool decode_(int,int);
};
@DECODE@
}
int main(int argc,char**argv){
 assert(argc==6);std::ifstream input(argv[1],std::ios::binary);
 std::vector<uint8_t>jpeg{std::istreambuf_iterator<char>(input),{}};
 RtpJpegFrameView frame;assert(parse_jpeg_for_rtp(jpeg.data(),jpeg.size(),&frame));
 const int tx=socket(AF_INET,SOCK_DGRAM,0),rx=socket(AF_INET,SOCK_DGRAM,0);assert(tx>=0&&rx>=0);
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(rx,reinterpret_cast<sockaddr*>(&address),sizeof address)==0);
 socklen_t length=sizeof address;assert(getsockname(rx,reinterpret_cast<sockaddr*>(&address),&length)==0);
 timeval timeout{2,0};assert(setsockopt(rx,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)==0);
 std::vector<uint8_t>received(131072);RtpJpegDepacketizer dep;
 std::array<uint8_t,RtpJpegDepacketizer::kQuantizationCacheBytes>cache{};
 dep.set_quantization_cache(cache.data(),cache.size());
 size_t completed=0,fragments=0;JpegFrameInfo info{};uint16_t sequence=60000;
 for(size_t offset=0;offset<frame.scan_size;){
  std::array<uint8_t,1200>packet{};
  const size_t header=build_rtp_jpeg_fragment_header(frame,offset,packet.data()+12,packet.size()-12);
  assert(header);size_t count=std::min<size_t>(173,frame.scan_size-offset); // Force fragmentation.
  const bool last=offset+count==frame.scan_size;
  packet[0]=0x80;packet[1]=26|(last?128:0);packet[2]=sequence>>8;packet[3]=sequence++;
  packet[4]=0;packet[5]=1;packet[6]=0x5f;packet[7]=0x90;packet[11]=7;
  memcpy(packet.data()+12+header,frame.scan+offset,count);const size_t bytes=12+header+count;
  assert(sendto(tx,packet.data(),bytes,0,reinterpret_cast<sockaddr*>(&address),sizeof address)==ssize_t(bytes));
  std::array<uint8_t,1200>wire{};const auto n=recv(rx,wire.data(),wire.size(),0);
  assert(n==ssize_t(bytes)&&memcmp(wire.data(),packet.data(),bytes)==0);
  assert(wire[0]==0x80&&(wire[1]&127)==26);
  const uint32_t timestamp=(uint32_t(wire[4])<<24)|(uint32_t(wire[5])<<16)|(uint32_t(wire[6])<<8)|wire[7];
  assert(timestamp==90000);const auto status=dep.push(wire.data()+12,n-12,wire[1]&128,timestamp,
    received.data(),received.size(),&completed,&info);
  assert(status==(last?RtpJpegPushResult::COMPLETE:RtpJpegPushResult::INCOMPLETE));
  offset+=count;fragments++;
 }
 assert(close(tx)==0&&close(rx)==0&&completed&&info.width==frame.width&&info.height==frame.height);
 std::ofstream restored(std::string(argv[2])+".jpg",std::ios::binary);
 restored.write(reinterpret_cast<char*>(received.data()),completed);restored.close();
 const unsigned width=std::atoi(argv[3]),height=std::atoi(argv[4]);
 std::vector<uint8_t>surface(width*height*2+32,0xa5);
 esphome::esp_video_renderer::EspVideoRenderer renderer;
 renderer.inputs_[0]={received.data(),completed};renderer.surfaces_[0].data=surface.data()+16;
 renderer.width_=width;renderer.height_=height;renderer.surface_capacity_=width*height*2;
 assert(renderer.decode_(0,0));assert(renderer.decoded_.load()==1&&renderer.decode_errors_.load()==0);
 assert(renderer.surfaces_[0].width==width&&renderer.surfaces_[0].height==height);
 for(unsigned i=0;i<16;i++)assert(surface[i]==0xa5&&surface[surface.size()-1-i]==0xa5);
 std::ofstream output(argv[2],std::ios::binary);output.write(reinterpret_cast<char*>(surface.data()+16),width*height*2);output.close();
 // Complete header with invalid Huffman selectors passes structural admission,
 // but must fail the actual decoder and never publish another surface.
 for(size_t i=2;i+9<completed;i++)if(received[i]==0xff&&received[i+1]==0xda){received[i+6]=0x33;break;}
 assert(!renderer.decode_(0,0)&&renderer.decoded_.load()==1&&renderer.decode_errors_.load()==1);
 renderer.inputs_[0].size=completed-2;assert(!renderer.decode_(0,0));
 assert(renderer.decoded_.load()==1&&renderer.decode_errors_.load()==2);
 std::cout<<"{\"fragments\":"<<fragments<<",\"jpeg_bytes\":"<<completed
  <<",\"width\":"<<width<<",\"height\":"<<height<<",\"decoded\":1,\"rejected\":2}";
}
'''.replace("@WORKSPACE@", workspace).replace("@UPDATE@", update).replace("@DECODE@", decode))
    binary = temporary / "media"
    component = ROOT / "esphome/components/voip_stack"
    result = subprocess.run(["g++", "-std=c++17", "-no-pie", *common,
        "-DUSE_ESPHOME_VOIP_STACK_VIDEO", "-DUSE_ESPHOME_VOIP_STACK_VIDEO_JPEG",
        "-I", str(ROOT), "-I", str(renderer), str(cpp), str(component / "rtp_jpeg.cpp"),
        *objects, "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("subsampling", [1, 2], ids=["422", "420"])
@pytest.mark.parametrize("scale", [1, 2, 4], ids=["native", "half", "quarter"])
def test_real_udp_jpeg_and_shipped_renderer_decode(real_jpeg_binary, tmp_path, scale, subsampling):
    # Large flat patches make the independent pixel oracle insensitive to the
    # different, valid chroma interpolation algorithms used by libjpeg/TJpgDec.
    width, height = 320, 240
    colors = [(216, 48, 32), (24, 204, 64), (40, 64, 224), (200, 192, 48)]
    image = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), colors[(y >= height // 2) * 2 + (x >= width // 2)])
    encoded = tmp_path / "input.jpg"
    image.save(encoded, "JPEG", quality=80, subsampling=subsampling, optimize=False, progressive=False)
    output = tmp_path / "rgb565"
    result = subprocess.run([real_jpeg_binary, encoded, output, str(width // scale), str(height // scale), "0"],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    receipt = json.loads(result.stdout)
    assert receipt["fragments"] > 1 and receipt["decoded"] == 1 and receipt["rejected"] == 2
    with Image.open(str(output) + ".jpg") as restored, Image.open(encoded) as original:
        assert restored.size == original.size == (width, height)
        assert restored.convert("RGB").tobytes() == original.convert("RGB").tobytes()
        reference = original.convert("RGB")
    pixels = output.read_bytes()
    assert len(pixels) == width * height * 2 // scale**2
    for x, y in [(width // 4, height // 4), (3 * width // 4, height // 4),
                 (width // 4, 3 * height // 4), (3 * width // 4, 3 * height // 4)]:
        offset = ((y // scale) * (width // scale) + x // scale) * 2
        rgb565 = int.from_bytes(pixels[offset:offset + 2], "little")
        actual = ((rgb565 >> 11) * 255 // 31, ((rgb565 >> 5) & 63) * 255 // 63, (rgb565 & 31) * 255 // 31)
        assert max(abs(a - b) for a, b in zip(actual, reference.getpixel((x, y)))) <= 10
    (tmp_path / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
