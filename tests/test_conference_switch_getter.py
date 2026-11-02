"""The optional conference setting remains a borrowed nullable switch."""
import subprocess
import pytest
from test_refer_dialog_runtime import SOURCE, block
from test_sip_registration_transport import conditional_body


@pytest.mark.parametrize("enabled", [False, True])
def test_conference_switch_getter_is_optional_and_borrowed(tmp_path, enabled):
    header = (SOURCE / "voip_stack.h").read_text()
    start = header.index("  // Optional switch-platform registration.")
    methods = conditional_body(header, start)
    code = r'''
#include <cassert>
#include <type_traits>
#include <utility>
namespace switch_ {struct Switch{unsigned callbacks=0;template<class F>void add_on_state_callback(F){++callbacks;}};}
struct Phone {
#ifdef USE_SWITCH
METHODS
 switch_::Switch *auto_answer_switch_=nullptr,*dnd_switch_=nullptr,*conference_ring_switch_=nullptr;
#endif
};
template<class T,class=void>struct HasGetter:std::false_type{};
template<class T>struct HasGetter<T,std::void_t<decltype(std::declval<const T&>().get_conference_ring_switch())>>:std::true_type{};
int main(){
#ifdef USE_SWITCH
 static_assert(HasGetter<Phone>::value);Phone p;const Phone&c=p;assert(c.get_conference_ring_switch()==nullptr);
 switch_::Switch setting;p.register_conference_ring_switch(&setting);assert(c.get_conference_ring_switch()==&setting);
 c.get_conference_ring_switch()->add_on_state_callback([](bool){});assert(setting.callbacks==1);
#else
 static_assert(!HasGetter<Phone>::value);
#endif
}
'''.replace("METHODS", methods)
    source = tmp_path / "switch.cpp"
    source.write_text(code)
    binary = tmp_path / "switch"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *(["-DUSE_SWITCH"] if enabled else []),
        str(source), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
