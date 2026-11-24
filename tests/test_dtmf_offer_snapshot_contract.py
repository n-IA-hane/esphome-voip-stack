"""The production offer snapshot owns directional telephone-event consent."""
from pathlib import Path
import subprocess
import pytest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'esphome/components/voip_stack'
@pytest.fixture(scope='module')
def snapshot_binary(tmp_path_factory):
    folder=tmp_path_factory.mktemp('dtmf-offer-snapshot')
    for name in ('esphome/core/defines.h','esphome/core/hal.h','esp_system.h','lwip/sockets.h'):
        header=folder/name
        header.parent.mkdir(parents=True,exist_ok=True)
        header.write_text('#include "sip_dialog_host_sdk.h"\n')
    binary=folder/'contract'
    result=subprocess.run(['g++','-std=c++17','-DUSE_ESP32','-DUSE_ESPHOME_VOIP_SIP_TRANSPORT','-DUSE_ESPHOME_VOIP_DTMF','-DUSE_ESPHOME_VOIP_STACK_PCM','-I',str(folder),'-I',str(ROOT/'tests/fixtures'),'-I',str(ROOT),'-I',str(SOURCE),str(ROOT/'tests/fixtures/dtmf_offer_snapshot_contract.cpp'),str(SOURCE/'sip_message.cpp'),str(SOURCE/'media_negotiator.cpp'),'-o',str(binary)],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    return binary
@pytest.mark.parametrize('case',['reserialized-103-104','canonical-uac','renumbered-answer','event-intersection','fallback-8000','missing-clock','retired-clock','introduced-clock','disjoint-events','duplicate-clock','duplicate-pt','audio-pt-collision','malformed-events','retained-alias-order','retired-alias','split-alias-event-masks'])
def test_serialized_offer_owns_dtmf_payload_and_events(snapshot_binary,case):
    result=subprocess.run([str(snapshot_binary),case],capture_output=True,text=True,timeout=5)
    assert result.returncode==0,result.stdout+result.stderr
