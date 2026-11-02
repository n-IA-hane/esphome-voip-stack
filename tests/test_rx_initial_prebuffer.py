"""Initial prebuffer notifications retry RX without an established-gap delay."""

from pathlib import Path
import subprocess

import pytest

from test_rx_playout_owner import owner_source

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def initial_prebuffer_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("rx-initial-prebuffer")
    helper = folder / "esphome/core/helpers.h"
    helper.parent.mkdir(parents=True)
    helper.write_text(
        "namespace esphome {struct Mutex {};"
        "struct LockGuard {explicit LockGuard(Mutex&){}};}\n"
    )
    source = owner_source()
    source = source.replace(
        "std::function<void()>on_start;", "std::function<void()>on_start,on_play;"
    ).replace("accepted+=bytes;return bytes;", "if(on_play)on_play();accepted+=bytes;return bytes;")
    marker = ' if(mode=="burst"){'
    assert source.count(marker) == 1
    source = source.replace(marker, r'''
 if(mode=="initial-prebuffer"||mode=="established-gap"){
  phone.call_state_=CallState::CONNECTING;phone.audio_devices_active_=true;
  phone.rx_playout_state_=tag(revision,Phase::ACTIVE);phone.format.sample_rate=16000;
  sink.info.rate=16000;
  // The captured startup packet dispatch spans4.331ms; no consumer invocation
  // is forced between packets. Notifications drive the real RX task's waits.
  const unsigned arrived_us[]={0,196,335,479,624,782,924,1064,2542,2733,
                               2886,3033,3192,3434,3596,3737,3893,4043,4184,4331};
  const bool initial=mode=="initial-prebuffer";
  unsigned next=0,first_pcm_ms=0,fifth_pcm_ms=0;
  auto enqueue=[&](){
   phone.on_audio_received_({pcm,320,uint16_t(next),next*160,true,false,pack_audio_format(phone.format)});
   ++next;
  };
  sink.on_play=[&](){
   if(!sink.accepted)first_pcm_ms=clock_ms;
   if(sink.accepted==4*320)fifth_pcm_ms=clock_ms;
  };
  const unsigned count=initial?20:5;
  for(unsigned n=0;n<(initial?1u:4u);++n)enqueue();
  assert(phone.first_audio_received_&&phone.audio_devices_active_);
  on_wait=[&](){
   if(next<count){
    clock_ms=initial?100+arrived_us[next]/1000:101;
    enqueue();skip_wait=true;return;
   }
   if(!jitter.depth())throw Paused{};
   // No further notification: advance to the requested deadline normally.
  };
  try{phone.rx_task_();}catch(const Paused&){}
  on_wait={};
  const auto counters=jitter.counters();
  fprintf(stderr,"mode=%s accepted=%u first_ms=%u fifth_ms=%u drops=%u discarded=%u\n",
          mode.c_str(),sink.accepted,first_pcm_ms,fifth_pcm_ms,counters.drops,counters.discarded_frames);
  assert(!counters.drops&&!counters.missing&&sink.accepted==count*320&&jitter.depth()==0);
  if(initial){
   assert(first_pcm_ms<=101&&"Initial prebuffer must retry on data notifications, before a full frame deadline");
  }else{
   assert(first_pcm_ms==100&&fifth_pcm_ms==110&&
          "An established stream must retain the full frame deadline despite a new notification");
  }
  return 0;
 }
''' + marker)
    cpp = folder / "owner.cpp"
    cpp.write_text("#include <cstdio>\n" + source)
    binary = folder / "owner"
    built = subprocess.run(
        ["g++", "-std=c++17", "-DUSE_ESP32", "-DUSE_ESPHOME_VOIP_STACK_SPEAKER",
         "-I", str(folder), "-I", str(ROOT), str(cpp), "-o", str(binary)],
        capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    return binary


@pytest.mark.parametrize("mode", ["initial-prebuffer", "established-gap"])
def test_real_rx_task_initial_prebuffer_and_established_deadline(initial_prebuffer_binary, mode):
    run = subprocess.run(
        ["bash", "-c", 'ulimit -c 0; exec "$1" "$2"', "bash", str(initial_prebuffer_binary), mode],
        capture_output=True, text=True, timeout=5,
    )
    assert run.returncode == 0, run.stderr
