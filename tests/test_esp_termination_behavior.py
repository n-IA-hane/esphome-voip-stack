from pathlib import Path
import subprocess
import textwrap


ROOT = Path(__file__).resolve().parents[1]


def test_call_termination_dispatcher_host_behavioral_contract(tmp_path: Path) -> None:
    probe = tmp_path / "termination_behavior.cpp"
    probe.write_text(
        textwrap.dedent(
            r"""
            #include "esphome/components/voip_stack/voip_fsm.h"
            #include <cassert>
            #include <vector>
            using namespace esphome::voip_stack;

            enum Event { CACHE = 1, CANCEL, BYE, FINAL_RESPONSE, END,
                         MEDIA_OFF, DISCONNECT };

            std::vector<int> run(CallState state, SipTerminationAction action,
                                 bool connected = true,
                                 bool has_call_id = true,
                                 bool send_ok = true,
                                 bool cache = true) {
              std::vector<int> events;
              const TerminationSnapshot snapshot{state, connected, has_call_id};
              const TerminationIntent intent{CallEndReason::LOCAL_HANGUP,
                                             "detail", action, cache};
              dispatch_call_termination(
                  snapshot, intent,
                  [&]() { events.push_back(CACHE); },
                  [&](SipTerminationAction resolved) {
                    events.push_back(resolved == SipTerminationAction::CANCEL
                                         ? CANCEL
                                         : resolved == SipTerminationAction::BYE
                                               ? BYE
                                               : FINAL_RESPONSE);
                    return resolved != SipTerminationAction::FINAL_RESPONSE &&
                           send_ok;
                  },
                  [&]() { events.push_back(END); },
                  [&]() { events.push_back(MEDIA_OFF); },
                  [&]() { events.push_back(DISCONNECT); });
              return events;
            }

            int main() {
              assert(run(CallState::IDLE, SipTerminationAction::AUTO).empty());
              assert(run(CallState::TERMINATING,
                         SipTerminationAction::AUTO).empty());
              assert(run(CallState::BUSY, SipTerminationAction::AUTO).empty());
              assert(run(CallState::DECLINED,
                         SipTerminationAction::AUTO).empty());
              assert(run(CallState::CANCELLED,
                         SipTerminationAction::AUTO).empty());
              assert(run(CallState::MEDIA_INCOMPATIBLE,
                         SipTerminationAction::AUTO).empty());
              assert(run(CallState::TRANSPORT_UNREACHABLE,
                         SipTerminationAction::AUTO).empty());
              assert(run(CallState::AUTH_REQUIRED_UNSUPPORTED,
                         SipTerminationAction::AUTO).empty());
              assert((run(CallState::CALLING, SipTerminationAction::AUTO) ==
                      std::vector<int>{CACHE, CANCEL, END, MEDIA_OFF}));
              assert((run(CallState::REMOTE_RINGING,
                          SipTerminationAction::AUTO) ==
                      std::vector<int>{CACHE, CANCEL, END, MEDIA_OFF}));
              assert((run(CallState::CONNECTING, SipTerminationAction::AUTO) ==
                      std::vector<int>{CACHE, CANCEL, END, MEDIA_OFF}));
              assert((run(CallState::RINGING, SipTerminationAction::AUTO) ==
                      std::vector<int>{CACHE, FINAL_RESPONSE, END, MEDIA_OFF,
                                       DISCONNECT}));
              assert((run(CallState::IN_CALL, SipTerminationAction::AUTO) ==
                      std::vector<int>{CACHE, BYE, END, MEDIA_OFF}));
              assert((run(CallState::IN_CALL, SipTerminationAction::NONE) ==
                      std::vector<int>{CACHE, END, MEDIA_OFF, DISCONNECT}));
              assert((run(CallState::RINGING, SipTerminationAction::BYE) ==
                      std::vector<int>{CACHE, BYE, END, MEDIA_OFF}));
              assert((run(CallState::CALLING, SipTerminationAction::CANCEL,
                          true, true, false) ==
                      std::vector<int>{CACHE, CANCEL, END, MEDIA_OFF,
                                       DISCONNECT}));
              assert((run(CallState::IN_CALL, SipTerminationAction::AUTO,
                          false) ==
                      std::vector<int>{CACHE, END, MEDIA_OFF, DISCONNECT}));
              assert((run(CallState::IN_CALL, SipTerminationAction::AUTO,
                          true, false) ==
                      std::vector<int>{END, MEDIA_OFF, DISCONNECT}));
              assert((run(CallState::IN_CALL, SipTerminationAction::AUTO,
                          true, true, true, false) ==
                      std::vector<int>{BYE, END, MEDIA_OFF}));
            }
            """
        ),
        encoding="utf-8",
    )
    binary = tmp_path / "termination_behavior"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT),
            str(probe),
            "-o",
            str(binary),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(binary)], check=True, cwd=ROOT)
