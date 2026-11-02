"""A default keepalive is a distinct wire record on the already registered flow."""
import subprocess

import pytest

from test_refer_dialog_runtime import ROOT, SOURCE


@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_default_options_occurs_without_changing_binding(tmp_path, transport):
    harness = (ROOT / "tests/fixtures/registration_engine.cpp").read_text()
    code = harness[:harness.index("int main(")] + r'''
int main(int argc,char**argv){
 assert(argc==2);Harness h(std::string(argv[1])=="tcp");
 h.manager=SipRegistration();h.manager.set_hooks(h.hooks);
 auto config=h.config();assert(h.manager.set_account(config,0));h.manager.tick(0);
 const auto first=h.first.receive();h.feed(h.first,h.success(first),1);
 const auto expiry=h.manager.account(0).binding_expires_ms,refresh=h.manager.account(0).next_ms;
 const auto fd=h.first.esp;sockaddr_in before{};socklen_t size=sizeof(before);
 assert(getsockname(fd,reinterpret_cast<sockaddr*>(&before),&size)==0);
 h.manager.tick(15001);char packet[8192];const auto count=recv(h.first.peer,packet,sizeof(packet),MSG_DONTWAIT);
 assert(count>0&&"15s default must send OPTIONS after successful REGISTER");
 const std::string ping(packet,count);assert(ping.rfind("OPTIONS ",0)==0);
 assert(field(ping,"Accept").empty()&&field(ping,"Content-Type").empty()&&field(ping,"Expires").empty());
 assert(field(ping,"Call-ID")==field(first,"Call-ID")&&field(ping,"CSeq")!=field(first,"CSeq"));
 h.feed(h.first,response(ping,405),15002);
 assert(h.manager.is_registered(config.id)&&h.manager.account(0).binding_expires_ms==expiry&&h.manager.account(0).next_ms==refresh);
 sockaddr_in after{};size=sizeof(after);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&after),&size)==0);
 assert(before.sin_addr.s_addr==after.sin_addr.s_addr&&before.sin_port==after.sin_port);
}
'''
    include = tmp_path / "esphome/core/defines.h"
    include.parent.mkdir(parents=True)
    include.write_text("")
    cpp = tmp_path / "keepalive.cpp"
    cpp.write_text(code)
    exe = tmp_path / "keepalive"
    result = subprocess.run(["g++", "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(tmp_path), "-I", str(SOURCE), str(cpp), str(SOURCE / "sip_registration.cpp"), "-lcrypto", "-o", str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([exe, transport], capture_output=True, text=True, timeout=6)
    assert result.returncode == 0, result.stderr
