"""Ordinary OPTIONS keepalive shares native registration ownership and sockets."""

from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module", params=[False, True], ids=["internal", "psram"])
def keepalive_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("registration-keepalive")
    include = folder / "esphome/core/defines.h"
    include.parent.mkdir(parents=True)
    include.write_text("")
    if request.param:
        (include.parent / "helpers.h").write_text('''#pragma once
#include <cstdlib>
inline int allocated=0;
namespace esphome {
template<class T> struct RAMAllocator {
 using value_type=T; enum{ALLOC_EXTERNAL=1};
 explicit RAMAllocator(int flags=0):flags_(flags){}
 T* allocate(size_t n){++allocated;return static_cast<T*>(std::malloc(n*sizeof(T)));}
 void deallocate(T*p,size_t){std::free(p);} int flags_;
};
}
''')
    source = ROOT / "esphome/components/voip_stack"
    binary = folder / "keepalive"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-DUSE_ESPHOME_VOIP_REGISTRATION",
        *(["-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM"] if request.param else []), "-I", str(folder), "-I", str(source),
        str(source / "sip_registration.cpp"), str(ROOT / "tests/fixtures/registration_keepalive.cpp"),
        "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("scenario", ["default", "disabled", "identity", "timer", "timeout", "priority",
    "unregister", "mapping", "credentials", "loss", "allocation"])
def test_keepalive_native_owner_and_wire(keepalive_binary, transport, scenario):
    result = subprocess.run([keepalive_binary, scenario, transport], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("transport", ["udp", "tcp"])
@pytest.mark.parametrize("status", [200, 401, 403, 405, 407, 501, 699])
def test_every_valid_final_response_proves_flow_without_authentication(keepalive_binary, transport, status):
    result = subprocess.run([keepalive_binary, "final", transport, str(status)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
