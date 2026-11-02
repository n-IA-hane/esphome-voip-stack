"""Actual LDAP stage/commit owners over independent TCP and verified TLS peers."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import socket
import ssl
import subprocess

import pytest

from phone_client_openssl_adapter import write_adapter
from test_refer_dialog_runtime import block

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def owner_binary(tmp_path_factory):
    folder = tmp_path_factory.mktemp("ldap-owner-wire")
    adapter = write_adapter(folder)
    source = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "esphome/components/ldap_directory/directory_contact.h"
#include "esphome/components/voip_stack/phonebook.h"

#define ESP_LOGW(...) ((void)0)
namespace esphome::voip_stack {
constexpr size_t VOIP_STACK_MAX_ROUTE_ID_LEN=96;
URI_METHODS
class VoipStack {
 public:
 Phonebook phonebook_;
 struct DirectoryProvider {const void*owner=nullptr;std::string account_id;};
 std::array<DirectoryProvider,4> directory_providers_{};
 std::string device_route_id_="self";
 unsigned publications=0;
 void publish_phonebook_changed_(){++publications;}
 DirectoryCommitResult commit_contact_directory(const void*,std::vector<ContactEntry>,const std::string&);
 DirectoryCommitResult commit_contact_directory(const void*,ContactEntry*,size_t,const std::string&);
};
COMMIT_METHODS
}
namespace esphome::ldap_directory {
uint32_t millis(){return static_cast<uint32_t>(phone_client::monotonic_ms());}
struct Component {
 bool warning=false;unsigned timers=0;
 template<class F>void set_timeout(const char*,uint32_t,F){++timers;}
 void cancel_timeout(const char*){}
 void status_clear_warning(){warning=false;}
 void status_set_warning(){warning=true;}
};
struct Trigger {unsigned calls=0;void trigger(uint32_t){++calls;}};
// Platform/scheduling adapter only. Stage-owning production methods below are
// unchanged. The network and Phonebook collaborators are actual production.
class LdapDirectory:public Component {
 public:
 struct Configuration {
  std::string host="localhost",base_dn="ou=phonebook,dc=voip",bind_dn="uid=reader",filter="(cn=*)",username="reader",password="fixture-password",ca_pem,sip_host="pbx.example.test";
  uint16_t port=389,sip_port=5060;bool tls=false,sip_tcp=false,credentials_ready=true;
 }config_;
 std::mutex mutex_;
 std::atomic<uint32_t> generation_{1};
 uint32_t timeout_ms_=1000,refresh_ms_=300000,last_success_=0,result_generation_=0,result_code_=0;
 bool active_=true,enabled_=true,pending_result_=false,ready_=true,managed_by_ha_=false;
 uint8_t max_entries_=4;
 char receive_buffer_[8192]{};size_t receive_size_=sizeof receive_buffer_;
 voip_stack::ContactEntry stage_storage_[4],*stage_=stage_storage_;
 size_t stage_count_=0,stage_capacity_=4,contact_count_=0;
 const char*status_="updating";std::string account_id_="ha";
 voip_stack::VoipStack*parent_;phone_client::TcpStream stream_;
 Trigger updated_,error_;unsigned wakes=0;
 explicit LdapDirectory(voip_stack::VoipStack&p):parent_(&p){}
 void lock_(){mutex_.lock();}void unlock_(){mutex_.unlock();}
 void disable_loop(){}bool is_failed()const{return false;}
 void wake_worker_(){++wakes;}void notify_main_(){}
 void interrupt_worker_(){++generation_;stream_.interrupt();}
 void worker_run_();void loop();void reset_stage_();void refresh();
};
OWNER_METHODS
}
int main(int argc,char**argv){
 assert(argc==5);using namespace esphome;
 const std::string mode=argv[1];voip_stack::VoipStack phone;ldap_directory::LdapDirectory directory(phone);
 directory.config_.port=std::stoi(argv[2]);directory.config_.tls=std::stoi(argv[3]);
 if(directory.config_.tls){std::ifstream file(argv[4]);directory.config_.ca_pem.assign(std::istreambuf_iterator<char>(file),{});}
 assert(phone.phonebook_.add_one("Manual|manual|192.0.2.9|5060|40000|sip_udp")==voip_stack::AddResult::Added);
 voip_stack::ContactEntry previous;previous.id="entry-old";previous.name="Old";previous.route="old";
 assert(phone.commit_contact_directory(&directory,&previous,1,"ha")==voip_stack::DirectoryCommitResult::APPLIED);
 const auto initial_publications=phone.publications;
 directory.worker_run_();assert(directory.pending_result_);
 // Network completion must not expose a partial snapshot before the main loop.
 assert(phone.phonebook_.size()==2&&phone.phonebook_.find("Old")&&phone.publications==initial_publications);
 if(mode=="stale")++directory.generation_;
 directory.loop();
 if(mode=="success"){
  assert(directory.updated_.calls==1&&directory.error_.calls==0&&std::string(directory.status_)=="ready");
  assert(phone.phonebook_.size()==2&&!phone.phonebook_.find("Old")&&phone.phonebook_.find("Manual"));
  const auto*contact=phone.phonebook_.find_id("entry-new");
  assert(contact&&contact->name=="New"&&contact->number=="102"&&contact->kind==voip_stack::ContactKind::CONTACT&&contact->auto_blf&&contact->account_id=="ha");
  assert(phone.publications==initial_publications+1);
 }else if(mode=="empty"){
  assert(directory.updated_.calls==1&&phone.phonebook_.size()==1&&phone.phonebook_.find("Manual"));
  assert(phone.publications==initial_publications+1);
 }else{
  assert(phone.phonebook_.size()==2&&phone.phonebook_.find("Old")&&phone.publications==initial_publications);
  assert(directory.updated_.calls==0);
  if(mode=="stale")assert(directory.error_.calls==0&&directory.timers==0);
  else assert(directory.error_.calls==1&&std::string(directory.status_)=="error");
 }
 assert(!directory.pending_result_&&directory.stage_count_==0);
 directory.stream_.close();directory.stream_.close();
 std::cout<<"PASS "<<mode<<" publications="<<phone.publications-initial_publications<<" sizeof_contact="<<sizeof(voip_stack::ContactEntry)<<"\n";
}
'''
    core = ROOT / "esphome/components/voip_stack"
    uri = (core / "sip_message.cpp").read_text()
    source = source.replace("URI_METHODS", "\n".join(block(uri, marker) for marker in [
        "std::string trim_copy(", "static bool is_sip_uri_unreserved(", "static uint8_t hex_value(",
        "std::string sip_uri_user_encode(", "std::string sip_prefix_target_user(", "std::string sip_target_uri(",
    ]))
    settings = (core / "voip_settings.cpp").read_text()
    source = source.replace("COMMIT_METHODS", "\n".join(block(settings, marker) for marker in [
        "DirectoryCommitResult VoipStack::commit_contact_directory(const void *provider, std::vector",
        "DirectoryCommitResult VoipStack::commit_contact_directory(const void *provider, ContactEntry",
    ]))
    ldap = ROOT / "esphome/components/ldap_directory"
    owner = (ldap / "ldap_directory.cpp").read_text()
    source = source.replace("OWNER_METHODS", "\n".join(block(owner, marker) for marker in [
        "void LdapDirectory::worker_run_()", "void LdapDirectory::loop()",
        "void LdapDirectory::reset_stage_()", "void LdapDirectory::refresh()",
    ]))
    cpp = folder / "owner.cpp"
    cpp.write_text(source)
    binary = folder / "owner"
    built = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-pthread",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DUSE_ESP32",
        "-DUSE_ESPHOME_VOIP_DIRECTORY", "-DUSE_ESPHOME_VOIP_CONTACT_METADATA", "-DUSE_PHONE_CLIENT_TLS",
        "-I", str(folder), "-I", str(ROOT), str(cpp), str(adapter),
        str(ldap / "ldap_protocol.cpp"), str(ldap / "directory_contact.cpp"),
        str(ROOT / "esphome/components/phone_client/net_stream.cpp"),
        "-lssl", "-lcrypto", "-o", str(binary)], capture_output=True, text=True, timeout=90)
    assert built.returncode == 0, built.stderr
    return binary


@pytest.fixture(scope="module")
def tls_certificate(tmp_path_factory):
    folder = tmp_path_factory.mktemp("ldap-tls-certificate")
    certificate, key = folder / "certificate.pem", folder / "key.pem"
    result = subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(key), "-out", str(certificate), "-days", "1", "-subj", "/CN=localhost",
        "-addext", "subjectAltName=DNS:localhost"], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
    return certificate, key


def message(stream):
    from ldap3.protocol.rfc4511 import LDAPMessage
    from pyasn1.codec.ber.decoder import decode
    packet = bytearray()
    while True:
        data = stream.recv(1)
        if not data:
            raise EOFError("LDAP request truncated")
        packet.extend(data)
        try:
            decoded, remaining = decode(bytes(packet), asn1Spec=LDAPMessage())
        except Exception:
            continue
        assert not remaining
        return decoded


def response(mid, operation, code=0):
    from ldap3.protocol.rfc4511 import LDAPMessage
    from pyasn1.codec.ber.encoder import encode
    packet = LDAPMessage()
    packet["messageID"] = mid
    entry = packet["protocolOp"][operation]
    if operation == "searchResEntry":
        entry["object"] = "uid=entry-new,ou=phonebook,dc=voip"
        for index, (name, value) in enumerate({"uid": "entry-new", "cn": "New", "telephoneNumber": "102",
            "labeledURI": "sip:102@pbx.example.test", "ou": "contact"}.items()):
            entry["attributes"][index]["type"] = name
            entry["attributes"][index]["vals"][0] = value
    else:
        entry["resultCode"], entry["matchedDN"], entry["diagnosticMessage"] = code, "", ""
    return encode(packet)


@pytest.mark.parametrize("tls", [False, True], ids=["tcp", "verified-tls"])
@pytest.mark.parametrize("scenario", ["success", "empty", "limit", "truncated", "wrong-id", "stale"])
def test_real_peer_atomic_snapshot_failure_manual_and_generation(owner_binary, tls_certificate, tls, scenario):
    certificate, key = tls_certificate
    with socket.socket() as listener, ThreadPoolExecutor(max_workers=1) as executor:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(5)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)

        def peer():
            stream, _ = listener.accept()
            with stream:
                stream.settimeout(3)
                if tls:
                    stream = context.wrap_socket(stream, server_side=True)
                with stream:
                    bind = message(stream)
                    assert bind["protocolOp"].getName() == "bindRequest"
                    assert str(bind["protocolOp"]["bindRequest"]["authentication"]["simple"]) == "fixture-password"
                    stream.sendall(response(int(bind["messageID"]), "bindResponse"))
                    search = message(stream)
                    assert search["protocolOp"].getName() == "searchRequest"
                    assert int(search["protocolOp"]["searchRequest"]["sizeLimit"]) == 4
                    mid = int(search["messageID"])
                    if scenario != "empty":
                        stream.sendall(response(mid + (scenario == "wrong-id"), "searchResEntry"))
                    if scenario != "truncated":
                        stream.sendall(response(mid, "searchResDone", 4 if scenario == "limit" else 0))
                    if scenario in {"success", "empty", "stale"}:
                        assert message(stream)["protocolOp"].getName() == "unbindRequest"

        pending = executor.submit(peer)
        mode = scenario if scenario in {"success", "empty", "stale"} else "failure"
        result = subprocess.run([owner_binary, mode, str(listener.getsockname()[1]), str(int(tls)), certificate],
                                capture_output=True, text=True, timeout=6)
        pending.result(timeout=5)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.startswith("PASS " + mode)
