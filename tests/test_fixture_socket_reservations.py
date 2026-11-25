"""The native test allocator reserves both SIP protocols and consecutive RTP ports."""
from pathlib import Path
import subprocess


def test_reserved_ports_are_unavailable_until_explicit_handoff(tmp_path):
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = tmp_path / "ports.cpp"
    source.write_text(r'''
#include "socket_reservations.h"
bool available(uint16_t port,int type){
 int fd=socket(AF_INET,type,0);assert(fd>=0);sockaddr_in address{};
 address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_ANY);address.sin_port=htons(port);
 bool ok=bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;close(fd);return ok;
}
int main(){
 ReservedPorts sip(ReservedPorts::Kind::SIP),rtp(ReservedPorts::Kind::UDP),video(ReservedPorts::Kind::RTP_PAIR);
 assert(sip.port()!=rtp.port()&&sip.port()!=video.port()&&sip.port()!=video.port()+1);
 assert(!available(sip.port(),SOCK_DGRAM)&&!available(sip.port(),SOCK_STREAM));
 assert(!available(rtp.port(),SOCK_DGRAM));
 assert(!(video.port()&1U)&&!available(video.port(),SOCK_DGRAM)&&!available(video.port()+1,SOCK_DGRAM));
 sip.release();rtp.release();video.release();
 assert(available(sip.port(),SOCK_DGRAM)&&available(sip.port(),SOCK_STREAM));
 assert(available(rtp.port(),SOCK_DGRAM)&&available(video.port(),SOCK_DGRAM)&&available(video.port()+1,SOCK_DGRAM));
}
''')
    binary = tmp_path / "ports"
    built = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(fixtures),
                            str(source), "-o", str(binary)], capture_output=True, text=True, timeout=20)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([binary], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
