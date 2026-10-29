"""Compose real MWI state changes, aggregate query and deferred UI delivery."""

import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block


@pytest.fixture(scope="module")
def observer_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("mwi-observer")
    defines = folder / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    header = (SOURCE / "voip_stack.h").read_text()
    adapter = (SOURCE / "voip_registration.cpp").read_text()
    mailbox = (SOURCE / "voip_message_waiting.cpp").read_text()
    transport = (SOURCE / "sip_registration_transport.cpp").read_text()
    harness = (SOURCE.parents[2] / "tests/test_sip_mwi_engine.cpp").read_text()
    code = harness[:harness.index("int main(")] + r'''
#include <functional>
#include <memory>
#include <utility>
// Model only ESPHome scheduling/automation infrastructure. The MWI engine,
// aggregate query and component callback below are production code.
bool protocol_context = false;
struct LockGuard {
 explicit LockGuard(int &) { assert(!protocol_context); }
};
template<typename> struct LazyCallbackManager;
template<> struct LazyCallbackManager<void()> {
 std::vector<std::function<void()>> callbacks;
 template<typename F> void add(F &&fn) { callbacks.emplace_back(std::forward<F>(fn)); }
 void call() { for (auto &fn : callbacks) fn(); }
};
struct SipTransport {
 SipRegistration &registration_; SipMwi &mwi_; int dialog_mutex_ = 0;
 bool has_waiting_messages(const std::string &id = "");
};
struct VoipStack {
 std::unique_ptr<SipTransport> transport_;
 LazyCallbackManager<void()> message_waiting_callbacks_;
 struct Trigger {
  unsigned count = 0;
  void trigger(const std::string &, bool, bool, uint32_t, uint32_t, uint32_t, uint32_t) {
   assert(!protocol_context); ++count;
  }
 } message_waiting_trigger_;
 std::vector<std::function<void()>> deferred;
 void defer(std::function<void()> fn) { deferred.emplace_back(std::move(fn)); }
 void run_main_loop() {
  assert(!protocol_context);
  auto work = std::move(deferred); deferred.clear();
  for (auto &fn : work) fn();
 }
 bool has_waiting_messages(const std::string &id = "");
 static void message_waiting_callback_(void *, const MessageWaitingState &);
 void publish_message_waiting_(const MessageWaitingState &);
 REPLACE_OBSERVER
};
'''.replace("REPLACE_OBSERVER", block(header, "template<typename F> void add_on_message_waiting_callback("))
    code += "\n" + block(transport, "bool SipTransport::has_waiting_messages(")
    code += "\n" + block(mailbox, "bool VoipStack::has_waiting_messages(")
    code += "\n" + block(mailbox, "void VoipStack::publish_message_waiting_(")
    code += "\n" + block(adapter, "void VoipStack::message_waiting_callback_(")
    code += r'''
VoipStack *observer = nullptr;
int main(int argc, char **argv) {
 assert(argc == 2);
 Harness h(std::string(argv[1]) == "tcp");
 SipRegistration accounts;
 assert(accounts.set_account(h.config, 0));
 VoipStack component;
 component.transport_.reset(new SipTransport{accounts, h.client}); observer = &component;
 std::vector<bool> visible;
 component.add_on_message_waiting_callback([&]() {
  assert(!protocol_context);
  visible.push_back(component.has_waiting_messages());
 });
 h.hooks.notify = [](void *, const MessageWaitingState &state) {
  protocol_context = true;
  VoipStack::message_waiting_callback_(observer, state);
  protocol_context = false;
 };
 h.client.set_hooks(h.hooks);
 const auto first = h.active();
 assert(visible.empty());
 component.run_main_loop();
 assert(!visible.empty() && visible.back());
 assert(component.has_waiting_messages("desk"));

 auto other = h.config; other.id = "other"; other.username = "bob";
 assert(accounts.set_account(other, 3));
 h.client.update_account(1, other, INADDR_LOOPBACK, h.second.port,
                         "sip:bob@127.0.0.1:5060", true, true, 3);
 h.client.tick(3);
 const auto second = h.second.receive();
 assert(h.client.handle_response(h.second.deliver(response(second, 200, "Expires: 60\r\n")),
                                 INADDR_LOOPBACK, h.second.port, h.second.tcp, 1, 4));
 assert(h.client.handle_notify(h.second.deliver(notify(second)), INADDR_LOOPBACK,
                               h.second.port, h.second.tcp, 1, 5));
 assert(h.second.receive().rfind("SIP/2.0 200", 0) == 0);
 component.run_main_loop();
 assert(component.has_waiting_messages("other"));

 // Clearing one mailbox must leave the aggregate lamp on for the other.
 h.client.update_account(0, h.config, INADDR_LOOPBACK, h.first.port,
                         "sip:alice@127.0.0.1:5060", true, false, 6);
 component.run_main_loop();
 assert(visible.back() && !component.has_waiting_messages("desk"));
 assert(component.has_waiting_messages("other"));

 // Transport loss clears stale data before notifying the main-loop viewer.
 h.client.connection_lost(1, 7);
 assert(!component.has_waiting_messages());
 const auto before_loss = visible.size();
 component.run_main_loop();
 assert(visible.size() == before_loss + 1 && !visible.back());

 h.client.update_account(1, other, INADDR_LOOPBACK, h.second.port,
                         "sip:bob@127.0.0.1:5060", true, true, 8);
 h.client.tick(8);
 const auto rebound = h.second.receive();
 assert(field(rebound, "Call-ID") != field(second, "Call-ID"));
 assert(!component.has_waiting_messages());
 assert(h.client.handle_response(h.second.deliver(response(rebound, 200, "Expires: 60\r\n")),
                                 INADDR_LOOPBACK, h.second.port, h.second.tcp, 1, 9));
 const auto fresh_notify = notify(rebound);
 assert(h.client.handle_notify(h.second.deliver(fresh_notify), INADDR_LOOPBACK,
                               h.second.port, h.second.tcp, 1, 10));
 assert(h.second.receive().rfind("SIP/2.0 200", 0) == 0);
 component.run_main_loop(); assert(visible.back());
 const auto before_duplicate = visible.size();
 assert(h.client.handle_notify(h.second.deliver(fresh_notify), INADDR_LOOPBACK,
                               h.second.port, h.second.tcp, 1, 11));
 assert(h.second.receive().rfind("SIP/2.0 200", 0) == 0);
 component.run_main_loop(); assert(visible.size() == before_duplicate);

 h.client.invalidate_account(1);
 assert(accounts.remove_account("other"));
 component.run_main_loop();
 assert(!visible.back() && !component.has_waiting_messages());
 assert(component.message_waiting_trigger_.count == visible.size());
}
'''
    source = folder / "observer.cpp"
    source.write_text(code)
    binary = folder / "observer"
    build = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DUSE_ESPHOME_VOIP_MWI",
        "-I", str(folder), "-I", str(SOURCE), str(source),
        str(SOURCE / "sip_mwi.cpp"), str(SOURCE / "sip_registration.cpp"),
        "-lcrypto", "-o", str(binary),
    ], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    return binary


@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_ui_observer_tracks_current_aggregate_after_mailbox_lifecycle(observer_binary, transport):
    result = subprocess.run([observer_binary, transport], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
