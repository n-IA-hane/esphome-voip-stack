"""Check the YAML condition against the production call-state predicate."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


def test_active_condition_preserves_terminal_state_semantics(tmp_path):
    header = (COMPONENT / "voip_stack.h").read_text()
    begin = header.index("  bool is_active() const {")
    end = header.index("\n  }", begin) + len("\n  }")
    method = header[begin:end]
    states = (COMPONENT / "voip_fsm.h").read_text()
    begin = states.index("enum class CallState")
    states = states[begin:states.index("};", begin) + 2]
    actions = (COMPONENT / "actions.h").read_text()
    begin = actions.index("template<typename... Ts>\nclass VoipIsActiveCondition")
    condition = actions[begin:actions.index("};", begin) + 2]
    probe = tmp_path / "active.cpp"
    probe.write_text("#include <atomic>\n#include <cassert>\n#include <cstdint>\n" + states + r'''
template<typename... Ts> struct Condition { virtual bool check(const Ts &...)=0; };
template<typename T> struct Parented { T *parent_{}; void set_parent(T *p){parent_=p;} };
struct VoipStack { std::atomic<CallState> call_state_{CallState::IDLE};
''' + method + "\n};\n" + condition + r'''
int main() {
  VoipStack phone;
  VoipIsActiveCondition<> condition;
  condition.set_parent(&phone);
  for (unsigned i=0;i<=static_cast<unsigned>(CallState::AUTH_REQUIRED_UNSUPPORTED);++i) {
    const auto state=static_cast<CallState>(i);
    phone.call_state_=state;
    const bool expected=state==CallState::CALLING || state==CallState::REMOTE_RINGING ||
      state==CallState::RINGING || state==CallState::CONNECTING || state==CallState::IN_CALL;
    assert(condition.check()==expected);
  }
  phone.call_state_=CallState::TERMINATING;
  assert(!condition.check());
  assert(phone.call_state_!=CallState::IDLE);
}
''')
    binary = tmp_path / "active"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(probe), "-o", str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True)


def test_actions_and_active_condition_resolve_an_unnamed_stack(tmp_path):
    source = (ROOT / "tests/minimal_no_text.yaml").read_text().replace(
        "../esphome/components", str(COMPONENT.parent)
    ).replace("  id: phone\n", "")
    source += '''
button:
  - platform: template
    name: Call control
    on_press:
      - if:
          condition:
            voip_stack.is_active:
          then:
            - voip_stack.stop:
          else:
            - voip_stack.start:
'''
    config = tmp_path / "unnamed.yaml"
    config.write_text(source)
    result = subprocess.run([sys.executable, "-m", "esphome", "compile", str(config), "--only-generate"], capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    assert "VoipIsActiveCondition" in generated
    assert "VoipStack()" in generated
