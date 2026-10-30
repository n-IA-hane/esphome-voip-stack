"""Execute the production DNS callback/owner handoff in a modeled TCPIP context."""
import subprocess

from test_refer_dialog_runtime import SOURCE, block


def test_dns_callback_never_uses_bsd_socket_context(tmp_path):
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    owner = (SOURCE / "voip_registration.cpp").read_text()
    header = (SOURCE / "sip_transport.h").read_text()
    context = block(adapter, "struct RegistrationDnsContext") + ";"
    found = block(adapter, "void registration_dns_found(")
    wake = block(adapter, "ctx.wake = []") + ";"
    ready = block(owner, "void VoipStack::registration_main_ready_callback_(")
    pump = block(owner, "void VoipStack::pump_registration_wake_(")
    resume = block(header, "void wake_registration_from_main_loop()")
    code = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <memory>
#include <cstdint>
#include <arpa/inet.h>
bool tcpip_context=false; unsigned socket_wakes=0, main_wakes=0;
int dns_mutex=1; constexpr int portMAX_DELAY=0;
void xSemaphoreTake(int,int){} void xSemaphoreGive(int){}
struct ip_addr_t{uint32_t addr;};
bool IP_IS_V4(const ip_addr_t*){return true;}
const ip_addr_t* ip_2_ip4(const ip_addr_t*p){return p;}
uint32_t ip4_addr_get_u32(const ip_addr_t*p){return p->addr;}
struct SipTransport {
 using Callback=void(*)(void*,SipTransport*);
 Callback registration_wake_callback_=nullptr; void*registration_context_=nullptr;
 std::atomic<bool> running_{true},transport_stopping_{false};
 void wake_sip_task_(){assert(!tcpip_context);++socket_wakes;}
 REPLACE_RESUME
};
struct VoipStack {
 std::unique_ptr<SipTransport> transport_;
 std::atomic<SipTransport*> registration_main_owner_{nullptr};
 void enable_loop_soon_any_context(){++main_wakes;}
 static void registration_main_ready_callback_(void*,SipTransport*);
 void pump_registration_wake_();
};
'''.replace("REPLACE_RESUME", resume)
    code += context + "\n" + found + "\n" + ready + "\n" + pump
    code += r'''
int main(){
 VoipStack component;component.transport_=std::make_unique<SipTransport>();
 auto*transport=component.transport_.get();
 transport->registration_context_=&component;
 transport->registration_wake_callback_=VoipStack::registration_main_ready_callback_;
 RegistrationDnsContext ctx;ctx.owner=transport;ctx.generation=1;ctx.request_generation=1;ctx.pending=true;
 REPLACE_WAKE
 ip_addr_t result{htonl(0x7f000001)};
 tcpip_context=true;registration_dns_found("pbx.example",&result,&ctx);
 assert(socket_wakes==0&&main_wakes==1&&ctx.ready&&!ctx.obsolete);
 tcpip_context=false;component.pump_registration_wake_();assert(socket_wakes==1);
 // A callback for the old transport may be queued while its owner replaces it.
 tcpip_context=true;registration_dns_found("pbx.example",&result,&ctx);
 auto previous=std::move(component.transport_);component.transport_=std::make_unique<SipTransport>();
 tcpip_context=false;component.pump_registration_wake_();assert(socket_wakes==1);
 // Cancellation detaches the static callback context before destruction.
 ctx.owner=nullptr;tcpip_context=true;registration_dns_found("pbx.example",&result,&ctx);
 assert(main_wakes==2&&socket_wakes==1);
 tcpip_context=false;component.pump_registration_wake_();assert(socket_wakes==1);
}
'''.replace("REPLACE_WAKE", wake)
    source = tmp_path / "dns.cpp"
    source.write_text(code)
    binary = tmp_path / "dns"
    build = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(source), "-o", str(binary)], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
