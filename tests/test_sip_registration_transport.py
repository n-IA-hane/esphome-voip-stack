"""Compose actual account ownership, dialog dispatch and TCP/UDP socket methods."""
from pathlib import Path
import subprocess

import pytest

from test_refer_wire_runtime import wire_source
from test_refer_dialog_runtime import SOURCE, block


def registration_transport_source(mwi=False):
    source = (SOURCE / "sip_transport.cpp").read_text()
    header = (SOURCE / "sip_transport.h").read_text()
    adapter = (SOURCE / "sip_registration_transport.cpp").read_text()
    code = wire_source()
    # Enable the new code after standard fixture includes have been declared.
    begin = header.index("  SipRegistration registration_;")
    members = header[begin:header.index("#endif", begin)]
    begin = header.index("  void initialize_registration();")
    public = header[begin:header.index("#endif", begin)]
    public = public.replace("void initialize_registration();", "")
    extra = public + "\n" + members + r'''
 void emit_connection_change_(bool){++connection_losses;}
 unsigned connection_losses=0;
 bool close_tcp_dialog_();
 void handle_tcp_peer_loss_(SipTcpFlow*);
 bool send_sip_tcp_record_(const std::string&s,int fd){return send_sip_tcp_record_(s.data(),s.size(),fd);}
'''
    if mwi:
        begin = header.index("  bool get_message_waiting(")
        extra += header[begin:header.index("#endif", begin)]
        begin = header.index("  SipMwi mwi_;")
        extra += header[begin:header.index("#endif", begin)]
    code = code.replace("void handle_tcp_peer_loss_(SipTcpFlow*f){close_tcp_flow_(*f);}", "")
    code += "\nvoid SipTransport::invalidate_registration_dns_(size_t) {}\n"
    names = ["registration_flow_index_", "registration_socket_index_", "account_in_use_", "retire_registration_flow_", "set_account", "remove_account",
             "register_account", "is_registered", "prepare_account_line", "clear_account_line", "registration_ingress_",
             "retry_authenticated_request_", "remember_registration_routes_", "registration_send_"]
    if mwi:
        names += ["park_inbound_control_", "registration_mwi_reply_"]
    for name in names:
        marker = next(line for line in adapter.splitlines() if "SipTransport::" + name + "(" in line)
        code += "\n" + block(adapter, marker)
    for marker in ["void SipTransport::handle_tcp_peer_loss_(", "bool SipTransport::close_tcp_dialog_("]:
        code += "\n" + block(source, marker)
    code = code.replace("using SipTcpFlow=::SipTcpFlow;", "using SipTcpFlow=::SipTcpFlow;\n" + extra)
    code = code.replace("struct SipTransport: SipPhoneTransport {", "struct SipTransport: SipPhoneTransport {\n")
    prefix = '''#include "sip_registration.h"
#ifdef USE_ESPHOME_VOIP_MWI
#include "sip_mwi.h"
#endif
#include <openssl/evp.h>
using esphome::voip_stack::SipRegistration;
#ifdef USE_ESPHOME_VOIP_MWI
using esphome::voip_stack::SipMwi;
using esphome::voip_stack::MessageWaitingState;
#endif
using esphome::voip_stack::SipDigest;
using esphome::voip_stack::SipAccountConfig;
using esphome::voip_stack::RegistrationState;
'''
    # The host fixture is global; production registration definitions keep
    # their normal namespace in the separately compiled translation unit.
    return prefix + code


def build_transport(root, mwi=False):
    defines = root / "esphome/core/defines.h"
    defines.parent.mkdir(parents=True)
    defines.write_text("")
    fixture = Path(__file__).parent / "fixtures/registration_transport.cpp"
    peers = (Path(__file__).parent / "fixtures/refer_wire_main.cpp").read_text().split("std::string begin(")[0]
    cpp = root / "transport.cpp"
    cpp.write_text(registration_transport_source(mwi) + "\n" + peers + "\n" + fixture.read_text())
    output = root / "transport"
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-g", "-fsanitize=address,undefined",
        *(["-DUSE_ESPHOME_VOIP_MWI"] if mwi else []),
        "-fno-omit-frame-pointer", "-DUSE_ESPHOME_VOIP_REGISTRATION", "-I", str(root), "-I", str(SOURCE.parents[2]), "-I", str(SOURCE),
        str(cpp), str(SOURCE / "sip_registration.cpp"), str(SOURCE / "sip_mwi.cpp"), "-lcrypto", "-o", str(output)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return output


@pytest.fixture(scope="module")
def registration_transport_binary(tmp_path_factory):
    return build_transport(tmp_path_factory.mktemp("sip-registration-transport"))


def test_notify_new_tcp_connection_while_direct_call_owns_main_flow(tmp_path):
    binary = build_transport(tmp_path, True)
    result = subprocess.run([binary, "notify-control-flow", "tcp"], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("scenario", ["authenticated-call", "foreign-flow", "atomic-change", "reset-missing", "refer-authenticated", "refer-direct-udp", "refer-direct-tcp"])
@pytest.mark.parametrize("transport", ["udp", "tcp"])
def test_registration_dialog_contracts(registration_transport_binary, scenario, transport):
    result = subprocess.run([registration_transport_binary, scenario, transport], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("target", ["udp", "tcp"])
def test_lost_account_connection_does_not_abort_a_valid_refer_candidate(registration_transport_binary, target):
    result = subprocess.run([registration_transport_binary, "refer-owner-loss-" + target, "tcp"], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("scenario", ["failed-flow-retry", "failed-flow-replace", "failed-flow-remove", "retired-flow-late-loss"])
def test_account_tcp_failed_mailbox_is_retired(registration_transport_binary, scenario):
    result = subprocess.run([registration_transport_binary, scenario, "tcp"], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("scenario", ["contact-next-hop", "proxy-next-hop", "route-next-hop"])
def test_confirmed_account_dialog_contact_proxy_and_route_are_distinct(registration_transport_binary, scenario):
    result = subprocess.run([registration_transport_binary, scenario, "udp"], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
