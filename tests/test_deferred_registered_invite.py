"""Public line selection through real deferred FSM and registered wire client."""
import subprocess

import pytest

from test_correlated_call_behavior import correlated_fixture_source, write_consumer_includes
from test_refer_dialog_runtime import ROOT, SOURCE, block
from test_sip_registration_transport import registration_transport_source


def app_source():
    # Reuse the existing application boundary fixture, retaining its real Phonebook.
    original = correlated_fixture_source()
    app = block(original, 'class VoipStack {') + ';'
    app = app.replace('Transport transport, *transport_=&transport;',
                      'std::unique_ptr<AppTransport> transport_=std::make_unique<AppTransport>();')
    app = app.replace('  unsigned terminations=0;', '''  unsigned terminations=0;
  std::string pending_line_,default_line_="ha";bool force_direct_call_=false;
  std::atomic<bool>audio_devices_active_{false};std::atomic<uint32_t>rx_playout_state_{0};void*rx_task_handle_=nullptr;
  bool is_active()const{return call_state_!=CallState::IDLE&&!call_state_is_terminating(call_state_);}
  void finish_call_termination_(){call_state_=CallState::IDLE;identity={};}
  void set_terminal_response_(const std::string&,const std::string&){}
  bool send_sip_cancel_(const std::string&id){return transport_->send_cancel_unlocked_(id);}
  bool send_sip_bye_(const std::string&id){return transport_->send_bye_unlocked_(id);}
  void send_sip_final_response_(const std::string&,const std::string&){}
  void end_call_(CallEndReason why,const std::string&){++terminations;termination_reason=why;terminated_identity=identity;call_state_=CallState::IDLE;identity={};}
  bool is_registered(const std::string&id){return transport_->is_registered(id);}
  bool select_line(const std::string&);
  void start_call_on_line(const std::string&,const std::string&);
  void start_call_on_line_(const std::string&,const std::string&,const std::string&);
''')
    for marker, replacement in [
        ('void request_call_termination_(', 'void request_call_termination_(const TerminationIntent&);'),
        ('void stop()', 'void stop();'),
        ('bool send_sip_invite_(', '''bool send_sip_invite_(const std::string&id,const std::string&cr,const std::string&cn,const std::string&dr,const std::string&dn){
          ++invites;return transport_->send_invite_unlocked_(id,cr,cn,dr,dn);}'''),
        ('void set_remote_sip_transport_tcp(', 'void set_remote_sip_transport_tcp(bool value){transport_->remote_sip_tcp_=value;}'),
        ('void set_remote_endpoint(', '''void set_remote_endpoint(const std::string&host,uint16_t port,uint16_t){
          in_addr addr{};assert(inet_aton(host.c_str(),&addr));transport_->remote_ip_v4_=ntohl(addr.s_addr);transport_->remote_sip_port_=port;}'''),
    ]:
        app = app.replace(block(app, marker), replacement)
    # The source's methods remain unchanged, including public line cleanup.
    fsm = (SOURCE / 'voip_fsm.cpp').read_text()
    settings = (SOURCE / 'voip_settings.cpp').read_text()
    registration = (SOURCE / 'voip_registration.cpp').read_text()
    methods = fsm[fsm.index('bool VoipStack::can_start_call_()'):fsm.index('void VoipStack::stop()')]
    methods += settings[settings.index('void VoipStack::call(const std::string &value)'):settings.index('void VoipStack::next_contact()')]
    start=settings.index('#ifdef USE_ESPHOME_VOIP_DIRECTORY\nstd::string VoipStack::get_current_destination()')
    methods += settings[start:settings.index('void VoipStack::publish_destination_()',start)]
    for marker in ['void VoipStack::begin_application_media_start_(', 'void VoipStack::pump_application_media_start_(',
                   'void VoipStack::finish_application_media_start_(', 'void VoipStack::request_call_termination_(',
                   'void VoipStack::stop()']:
        methods += '\n' + block(fsm,marker)
    for marker in ['bool VoipStack::select_line(', 'void VoipStack::start_call_on_line(',
                   'void VoipStack::start_call_on_line_(']:
        methods += '\n' + block(registration,marker)
    _parser=(SOURCE/'sip_message.cpp').read_text()
    # URI helpers are defined in the existing wire fixture where available.
    return app + '\n' + methods


def build(folder):
    write_consumer_includes(folder)
    (folder/'esphome/core/defines.h').write_text('')
    code=registration_transport_source(full_invite=True)
    code=code.replace('std::string make_token(const char *prefix){return std::string(prefix)+"new";}',
                      'std::string make_token(const char *prefix){static unsigned token=0;return std::string(prefix)+std::to_string(++token);}')
    fixture=(ROOT/'tests/fixtures/registration_transport.cpp').read_text()
    peers=(ROOT/'tests/fixtures/refer_wire_main.cpp').read_text().split('std::string begin(')[0]
    helpers=fixture.split('int main(')[0].replace('"office"','"qa"').replace('config.username = "100"','config.username = "7101"')
    helpers=helpers.replace('hooks.random = esp_random;',
                            'hooks.random = +[]()->uint32_t{static uint32_t token=100;return ++token;};')
    code += '\n'+peers+'\n'+helpers
    parser=(SOURCE/'sip_message.cpp').read_text()
    for marker in ['static bool is_sip_uri_unreserved(', 'std::string sip_uri_user_encode(',
                   'std::string sip_prefix_target_user(', 'std::string sip_normalize_dial_target(']:
        if marker not in code:
            code += '\n'+block(parser,marker)
    code += r'''
// Only the ESP application speaker boundary is deterministic. Socket I/O,
// REGISTER, digest challenge processing and INVITE/BYE/CANCEL are production.
#define USE_ESP32
#define USE_ESPHOME_VOIP_STACK_SPEAKER
#include "phonebook.h"
#include "voip_fsm.h"
#include <iostream>
#include <functional>
#define ESP_LOGE(...) ((void)0)
using namespace esphome::voip_stack;
void xTaskNotifyGive(void*){}
struct SipTransportSnapshot {uint32_t remote_ip_v4;uint16_t remote_sip_port;bool sip_tcp;bool terminal_transaction_pending;};
struct AppTransport: SipTransport {
 bool originate(const std::string&host,uint16_t port){in_addr addr{};assert(inet_aton(host.c_str(),&addr));remote_ip_v4_=ntohl(addr.s_addr);remote_sip_port_=port;return true;}
 void set_sip_signaling_transport(bool value){remote_sip_tcp_=value;}
 void set_peer_directional_audio_v1(bool value){peer_directional_audio_v1_=value;}
 SipTransportSnapshot snapshot()const{return {remote_ip_v4_.load(),remote_sip_port_.load(),remote_sip_tcp_.load(),false};}
 bool is_connected()const{return !call_id_.empty();}
 void disconnect(){reset_dialog_();}
 bool prepare_media_path(){return true;}bool commit_media_path(){return true;}void abort_media_path(){}
};
struct Trigger{unsigned calls=0;template<class...T>void trigger(T...){++calls;}};
'''+app_source()+r'''
void bind_line(SipTransport&s,Peer&peer,size_t slot,const std::string&id,const std::string&user){
 if(peer.tcp)s.registration_flows_[slot].socket=peer.esp;
 SipAccountConfig c;c.id=id;c.registrar="127.0.0.1";c.domain="pbx.example";
 c.username=user;c.password="test-only-password";c.tcp=peer.tcp;c.port=ntohs(peer.address.sin_port);
 assert(s.registration_.set_account(c,clock_ms));s.registration_.tick(clock_ms);
 auto request=pick(peer.drain(),"REGISTER");auto answer=response(request,200);
 answer.insert(answer.find("Content-Length:"),"Contact: "+header_value(request,"Contact")+"\r\n");
 peer.send_to(s,answer);assert(s.registration_.is_registered(id));
}
int main(int argc,char**argv){
 assert(argc==4);const std::string scenario=argv[1];unsigned status=std::stoul(argv[2]);bool tcp=std::string(argv[3])=="tcp";
 VoipStack app;auto&s=*app.transport_;Peer qa(tcp),ha(tcp);
 bind_account(s,qa); // actual REGISTER qa/7101 initializes the existing hooks/owner.
 bind_line(s,ha,1,"ha","home-user");
 app.sink_ready=false;
 app.start_call_on_line_("7102","qa","delayed-qa");
 assert(app.call_state_==CallState::CALLING&&qa.drain().empty());
 assert(app.pending_media_start_==VoipStack::ApplicationMediaStart::ORIGINATE);
 if(scenario=="abort"||scenario=="timeout"){
  if(scenario=="abort")app.stop();else{clock_ms+=5001;app.pump_application_media_start_();}
  assert(app.call_state_==CallState::IDLE&&s.next_account_==-1);
  app.sink_ready=true;app.call("home-target");
  auto invite=pick(ha.drain(),"INVITE");assert(header_value(invite,"From").find("home-user@pbx.example")!=std::string::npos);
  assert(s.active_account_==1&&qa.drain().empty());
 }else{
  assert(app.select_line("ha"));
  app.start_call_on_line_("not-admitted","ha","collision");
  app.sink_ready=true;app.pump_application_media_start_();
  auto first=pick(qa.drain(),"INVITE");
  std::cout<<"initial_from="<<header_value(first,"From")<<std::endl;
  qa.send_to(s,response(first,status));auto records=qa.drain();
  assert(!pick(records,"ACK").empty());
  std::string retried;
  for(const auto&m:records)if(m.rfind("INVITE ",0)==0)retried=m;
  assert(!retried.empty()&&"REGISTER-active delayed line lost: INVITE challenge has ACK but no authenticated retry");
  assert(header_value(retried,"From").find("7101@pbx.example")!=std::string::npos);
  assert(header_value(retried,status==401?"Authorization":"Proxy-Authorization").find("username=\"7101\"")!=std::string::npos);
  assert(header_value(retried,"Call-ID")==header_value(first,"Call-ID")&&cseq_number(header_value(retried,"CSeq"))>cseq_number(header_value(first,"CSeq")));
  assert(via_branch(header_value(retried,"Via"))!=via_branch(header_value(first,"Via")));
  assert(message_body(retried)==message_body(first)&&s.active_account_==0&&ha.drain().empty());
  qa.send_to(s,response(retried,200));assert(!pick(qa.drain(),"ACK").empty());
  assert(s.send_bye_unlocked_(s.call_id_));auto bye=pick(qa.drain(),"BYE");qa.send_to(s,response(bye,200));
  assert(s.call_id_.empty()&&s.active_account_==-1&&s.registration_.is_registered("qa")&&s.registration_.is_registered("ha"));
  app.stop();app.call("home-target");auto home=pick(ha.drain(),"INVITE");
  assert(header_value(home,"From").find("home-user@pbx.example")!=std::string::npos);
  assert(s.active_account_==1&&header_value(home,"Authorization").empty()&&qa.drain().empty());
 }
 for(auto&flow:s.registration_flows_)s.close_tcp_flow_(flow);close(s.sip_socket_);
}
'''
    cpp=folder/'deferred.cpp'
    cpp.write_text(code)
    binary=folder/'deferred'
    built=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
        '-DUSE_ESPHOME_VOIP_REGISTRATION','-I',str(folder),'-I',str(ROOT),'-I',str(SOURCE),str(cpp),
        str(SOURCE/'sip_registration.cpp'),'-lcrypto','-o',str(binary)],capture_output=True,text=True)
    (folder/'compile.stderr').write_text(built.stderr)
    assert built.returncode==0,built.stderr
    return binary


@pytest.fixture(scope='module')
def deferred_binary(tmp_path_factory):
    return build(tmp_path_factory.mktemp('deferred-account'))


@pytest.mark.parametrize('transport',['udp','tcp'])
@pytest.mark.parametrize('status',[401,407])
def test_registered_deferred_invite_retries_on_its_original_account(deferred_binary,transport,status):
    run=subprocess.run([deferred_binary,'retry',str(status),transport],capture_output=True,text=True,timeout=10)
    assert run.returncode==0,run.stdout+run.stderr


@pytest.mark.parametrize('transport',['udp','tcp'])
@pytest.mark.parametrize('scenario',['abort','timeout'])
def test_preinvite_abort_releases_line_before_default_ha_call(deferred_binary,transport,scenario):
    run=subprocess.run([deferred_binary,scenario,'401',transport],capture_output=True,text=True,timeout=10)
    assert run.returncode==0,run.stdout+run.stderr
