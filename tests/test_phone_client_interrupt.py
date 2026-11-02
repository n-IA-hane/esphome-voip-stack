"""Descriptor ownership and cancellation witnesses, with real host sockets."""
from pathlib import Path
import socket
import subprocess
import threading

import pytest

from phone_client_openssl_adapter import write_adapter

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def interrupt_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("phone-interrupt")
    adapter = write_adapter(folder)
    cpp = folder / "interrupt.cpp"
    cpp.write_text(r'''
#include "esphome/components/phone_client/net_stream.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace std::chrono_literals;
std::mutex gate;std::condition_variable condition;
bool entered=false,released=false,use_gate=true;int observed_fd=-1;
extern "C" int __real_shutdown(int,int);
// Stop at the syscall boundary after production interrupt has chosen its fd.
// Do not simulate shutdown or success: the actual kernel syscall follows.
extern "C" int __wrap_shutdown(int fd,int how){
 if(!use_gate)return __real_shutdown(fd,how);
 std::unique_lock<std::mutex> lock(gate);observed_fd=fd;entered=true;condition.notify_all();
 condition.wait(lock,[]{return released;});lock.unlock();return __real_shutdown(fd,how);
}
int main(int argc,char**argv){
 assert(argc==3);const std::string mode=argv[1];
 esphome::phone_client::TcpStream stream;
 if(mode=="handshake"){
  use_gate=false;
  std::thread control([&]{std::this_thread::sleep_for(50ms);stream.interrupt();});
  const auto start=std::chrono::steady_clock::now();
  const bool connected=stream.connect("localhost",std::stoi(argv[2]),1000,true);
  control.join();const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"handshake_ms="<<elapsed<<" connected="<<connected<<std::endl;
  assert(!connected&&elapsed<300&&"interrupt did not wake the active TLS handshake");return 0;
 }
 assert(stream.connect("localhost",std::stoi(argv[2]),1000));
 std::thread control([&]{stream.interrupt();});
 {std::unique_lock<std::mutex> lock(gate);assert(condition.wait_for(lock,1s,[]{return entered;}));}
 std::atomic<bool> closed=false;int victim[2]{-1,-1};
 std::thread owner([&]{
  stream.close();assert(socketpair(AF_UNIX,SOCK_STREAM,0,victim)==0);
  if(victim[0]!=observed_fd){assert(dup2(victim[0],observed_fd)==observed_fd);close(victim[0]);victim[0]=observed_fd;}
  closed=true;
 });
 // The close may complete only when no outstanding interrupt owns the old fd.
 for(unsigned i=0;i<50&&!closed;++i)std::this_thread::sleep_for(1ms);
 const bool reused_before_shutdown=closed;
 {std::lock_guard<std::mutex> lock(gate);released=true;}condition.notify_all();
 control.join();owner.join();
 const auto sent=send(victim[1],"x",1,MSG_NOSIGNAL);char byte=0;
 const auto received=recv(victim[0],&byte,1,MSG_DONTWAIT);
 std::cout<<"reused_before_shutdown="<<reused_before_shutdown<<" victim_send="<<sent<<" victim_recv="<<received<<std::endl;
 assert(sent==1&&received==1&&byte=='x'&&"interrupt shut down an unrelated reused descriptor");
 close(victim[0]);close(victim[1]);stream.close();
}
''')
    binary = folder / "interrupt"
    built = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-DUSE_PHONE_CLIENT_TLS", "-I", str(folder), "-I", str(ROOT),
        str(cpp), str(adapter), str(ROOT / "esphome/components/phone_client/net_stream.cpp"),
        "-Wl,--wrap=shutdown", "-lssl", "-lcrypto", "-o", str(binary)],
        capture_output=True, text=True, timeout=90)
    assert built.returncode == 0, built.stderr
    return binary


def test_interrupt_cannot_shutdown_reused_descriptor(interrupt_binary):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        result = subprocess.run([interrupt_binary, "reuse", str(listener.getsockname()[1])],
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, result.stdout + result.stderr


def test_interrupt_wakes_active_tls_handshake(interrupt_binary):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(3)
        done = threading.Event()

        def silent_peer():
            stream, _ = listener.accept()
            with stream:
                done.wait(2)

        peer = threading.Thread(target=silent_peer)
        peer.start()
        try:
            result = subprocess.run([interrupt_binary, "handshake", str(listener.getsockname()[1])],
                                    capture_output=True, text=True, timeout=4)
        finally:
            done.set()
            peer.join(timeout=3)
        assert not peer.is_alive()
        assert result.returncode == 0, result.stdout + result.stderr
