"""Bound SUBSCRIBE transient C++ allocations without retaining a renewal cache."""

import json
from pathlib import Path
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "esphome/components/voip_stack"


def test_subscribe_uses_transaction_storage_and_releases_it(tmp_path):
    headers = tmp_path / "esphome/core"
    headers.mkdir(parents=True)
    (headers / "defines.h").write_text("")
    # Tag the production signaling allocator separately from normal C++ new.
    # This is an allocation-policy host oracle, not a total ESP heap measure.
    (headers / "helpers.h").write_text(r"""#pragma once
#include <new>
namespace allocation_probe { extern thread_local bool external; }
namespace esphome {
template<class T> struct RAMAllocator {
  using value_type = T;
  enum { ALLOC_EXTERNAL = 1 };
  int flags_;
  explicit RAMAllocator(int flags = 0) : flags_(flags) {}
  template<class U> RAMAllocator(const RAMAllocator<U> &other) : flags_(other.flags_) {}
  T *allocate(size_t n) {
    const bool previous = allocation_probe::external;
    allocation_probe::external = flags_ == ALLOC_EXTERNAL;
    auto *result = static_cast<T *>(::operator new(n * sizeof(T)));
    allocation_probe::external = previous;
    return result;
  }
  void deallocate(T *p, size_t) { ::operator delete(p); }
  template<class U> bool operator==(const RAMAllocator<U> &other) const { return flags_ == other.flags_; }
  template<class U> bool operator!=(const RAMAllocator<U> &other) const { return !(*this == other); }
};
}
""")
    binary = tmp_path / "mwi-storage"
    built = subprocess.run(
        [
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
            "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI",
            "-DUSE_ESPHOME_VOIP_STACK_SIGNALING_PSRAM",
            "-I", str(tmp_path), "-I", str(SOURCE),
            str(ROOT / "tests/fixtures/mwi_request_storage.cpp"),
            str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_registration.cpp"),
            "-o", str(binary),
        ],
        capture_output=True, text=True,
    )
    assert built.returncode == 0, built.stderr
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    ran = subprocess.run(
        [binary], capture_output=True, text=True, timeout=10, preexec_fn=no_core,
    )
    assert ran.returncode == 0, ran.stderr
    rows = [json.loads(line) for line in ran.stdout.splitlines()]
    assert len(rows) == 9
    for row in rows:
        assert row["capacity_after_completion"] == row["empty_capacity"]
        if row["auth"] == 0:
            # A complete temporary internal wire string exceeds this budget.
            assert row["peak_internal_cpp_bytes_above_entry"] < row["wire_bytes"]
