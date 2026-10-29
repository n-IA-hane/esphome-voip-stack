"""Execute the native mailbox bridge with real aggregate and lifecycle methods."""

import subprocess

import pytest

from test_refer_dialog_runtime import SOURCE, block


@pytest.fixture(scope="module", params=[False, True], ids=["direct", "direct-and-sip"])
def native_mwi_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("native-mwi")
    owner = (SOURCE / "voip_message_waiting.cpp").read_text()
    adapter = (SOURCE / "ha_integration.h").read_text()
    code = r'''
#include "message_waiting.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
using esphome::voip_stack::MessageWaitingState;
namespace socket { constexpr size_t SOCKADDR_STR_LEN = 64; }
namespace api {
struct APIConnection {
 std::string name, address; bool authenticated;
 bool is_authenticated() { return authenticated; }
 const char *get_name() const { return name.c_str(); }
 const char *get_peername_to(char *buffer) const {
  std::strcpy(buffer, address.c_str()); return buffer;
 }
};
struct APIServer {
 std::vector<std::unique_ptr<APIConnection>> clients;
 const auto &active_clients() const { return clients; }
 APIConnection *connect(std::string name, std::string address, bool authenticated = true) {
  clients.emplace_back(new APIConnection{std::move(name), std::move(address), authenticated});
  return clients.back().get();
 }
 void disconnect(APIConnection *client) {
  auto it = std::find_if(clients.begin(), clients.end(), [=](const auto &p) { return p.get() == client; });
  assert(it != clients.end()); clients.erase(it);
 }
};
APIServer *global_api_server;
}
struct SipTransport {
 bool waiting = false;
 bool has_waiting_messages(const std::string &id) { return waiting && (id.empty() || id == "pbx"); }
 void get_message_waiting(const std::string &id, MessageWaitingState *state) {
  if (id == "pbx") { state->available = true; state->waiting = waiting; }
 }
};
struct VoipStack {
 std::unique_ptr<SipTransport> transport_{new SipTransport};
 uint32_t ha_new_messages_{0}, ha_old_messages_{0};
 bool ha_message_waiting_available_{false}, ha_message_waiting_{false};
 struct Trigger {
  std::vector<MessageWaitingState> events;
  void trigger(const std::string &id, bool available, bool waiting, uint32_t n,
               uint32_t o, uint32_t un, uint32_t uo) {
   events.push_back({id, available, waiting, n, o, un, uo});
  }
 } message_waiting_trigger_;
 struct Callbacks {
  std::vector<std::function<void()>> callbacks;
  void call() { for (auto &callback : callbacks) callback(); }
 } message_waiting_callbacks_;
 std::vector<std::function<void()>> deferred;
 void defer(std::function<void()> fn) { deferred.emplace_back(std::move(fn)); }
 void loop() {
  auto callbacks = std::move(deferred); deferred.clear();
  for (auto &callback : callbacks) callback();
 }
 MessageWaitingState get_message_waiting(const std::string &);
 bool has_waiting_messages(const std::string &id = "");
 void publish_message_waiting_(const MessageWaitingState &);
 void set_ha_message_waiting(bool, bool, uint32_t, uint32_t);
};
struct VoipHAIntegration {
 VoipStack *parent_;
 api::APIConnection *message_waiting_source_{nullptr};
 REPLACE_SET
 REPLACE_DISCONNECT
 REPLACE_MATCH
};
'''
    for name, marker in [("SET", "void set_message_waiting("),
                         ("DISCONNECT", "void on_api_client_disconnected("),
                         ("MATCH", "bool matches_source_(")]:
        code = code.replace("REPLACE_" + name, block(adapter, marker))
    for marker in ["MessageWaitingState VoipStack::get_message_waiting(",
                   "bool VoipStack::has_waiting_messages(",
                   "void VoipStack::publish_message_waiting_(",
                   "void VoipStack::set_ha_message_waiting("]:
        code += "\n" + block(owner, marker)
    code += r'''
int main() {
 api::APIServer server; api::global_api_server = &server;
 VoipStack component; VoipHAIntegration adapter{&component};
 std::vector<bool> observed;
 component.message_waiting_callbacks_.callbacks.emplace_back([&]() {
  observed.push_back(component.has_waiting_messages());
 });
 const std::string ha_name = "Home Assistant exact version";
 const std::string ha_address = "192.0.2.15";
 auto *ha = server.connect(ha_name, ha_address);
 auto *debug = server.connect("Diagnostics", ha_address);
 server.connect(ha_name, "192.0.2.20", false);
 adapter.set_message_waiting(true, true, 2, 3, "Home Assistant", ha_address);
 adapter.set_message_waiting(true, true, 2, 3, ha_name, "192.0.2.99");
 adapter.set_message_waiting(true, true, 2, 3, ha_name, "192.0.2.20");
 adapter.set_message_waiting(true, true, -1, 3, ha_name, ha_address);
 component.loop(); assert(observed.empty());
 adapter.set_message_waiting(true, true, 2, 3, ha_name, ha_address);
 assert(component.has_waiting_messages("direct") && observed.empty());
 auto state = component.get_message_waiting("direct");
 assert(state.available && state.waiting && state.new_messages == 2 && state.old_messages == 3);
 component.loop(); assert(observed.size() == 1 && observed.back());
 adapter.set_message_waiting(true, true, 2, 3, ha_name, ha_address);
 component.loop(); assert(observed.size() == 1);

 server.disconnect(debug);
 adapter.on_api_client_disconnected("Diagnostics", ha_address);
 component.loop(); assert(observed.size() == 1 && component.has_waiting_messages("direct"));
 // Same-source reconnect can overlap: removal must retain the remaining owner.
 auto *replacement = server.connect(ha_name, ha_address);
 server.disconnect(ha);
 adapter.on_api_client_disconnected(ha_name, ha_address);
 component.loop(); assert(observed.size() == 1 && adapter.message_waiting_source_ == replacement);
 // A PBX's separate mailbox must survive loss of HA/direct ownership.
#ifdef USE_ESPHOME_VOIP_MWI
 component.transport_->waiting = true;
#endif
 server.disconnect(replacement);
 adapter.on_api_client_disconnected(ha_name, ha_address);
 assert(!component.has_waiting_messages("direct"));
 state = component.get_message_waiting("direct");
 assert(!state.available && !state.waiting && state.new_messages == 0 && state.old_messages == 0);
 component.loop(); assert(observed.size() == 2);
#ifdef USE_ESPHOME_VOIP_MWI
 assert(observed.back() && component.has_waiting_messages("pbx"));
#else
 assert(!observed.back() && !component.has_waiting_messages());
#endif
 auto *fresh = server.connect(ha_name, ha_address);
 adapter.set_message_waiting(true, true, 1, 4, ha_name, ha_address);
 component.loop(); assert(component.has_waiting_messages("direct"));
 adapter.set_message_waiting(false, true, 1, 4, ha_name, ha_address);
 assert(!component.has_waiting_messages("direct") && adapter.message_waiting_source_ == nullptr);
 state = component.get_message_waiting("direct");
 assert(!state.available && !state.waiting && state.new_messages == 0 && state.old_messages == 0);
 server.disconnect(fresh);
 adapter.on_api_client_disconnected(ha_name, ha_address);
 component.loop();
}
'''
    source = folder / "native-mwi.cpp"
    source.write_text(code)
    binary = folder / "native-mwi"
    build = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-DUSE_ESPHOME_VOIP_HA_MWI",
        *(["-DUSE_ESPHOME_VOIP_MWI"] if request.param else []),
        "-I", str(SOURCE), str(source), "-o", str(binary),
    ], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    return binary


def test_direct_mailbox_owns_only_its_api_connection(native_mwi_binary):
    result = subprocess.run([native_mwi_binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
