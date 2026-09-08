"""Exercise the production allocator, including OOM before object construction."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_transport_control_storage_is_internal_and_oom_is_safe(tmp_path):
    source = (ROOT / "esphome/components/voip_stack/sip_transport.cpp").read_text()
    methods = source[
        source.index("void *SipTransport::operator new(") : source.index(
            "SipTransport::SipTransport("
        )
    ]
    cpp = tmp_path / "allocation.cpp"
    cpp.write_text(
        r"""
#include <cassert>
#include <cstdlib>
#include <memory>
#include <cstddef>
constexpr unsigned MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;
bool fail=false;
unsigned caps_seen=0, allocations=0, releases=0, constructors=0, destructors=0;
void *heap_caps_malloc(size_t size, unsigned caps) {
 caps_seen=caps; ++allocations; return fail ? nullptr : std::malloc(size);
}
void heap_caps_free(void *p) {++releases; std::free(p);}
class SipTransport {
 public:
  char storage[8192];
  SipTransport() {++constructors;}
  ~SipTransport() {++destructors;}
  static void *operator new(size_t) noexcept;
  static void operator delete(void *) noexcept;
};
"""
        + methods
        + r"""
int main() {
 {auto transport=std::make_unique<SipTransport>(); assert(transport);
  assert(caps_seen==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));}
 assert(allocations==1 && constructors==1 && destructors==1 && releases==1);
 fail=true;
 auto absent=std::make_unique<SipTransport>();
 assert(!absent && allocations==2 && constructors==1 && destructors==1 && releases==1);
}
"""
    )
    binary = tmp_path / "allocation"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(cpp),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
