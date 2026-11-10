"""Optional native queue declarations and bounded HA text configuration."""

import importlib
import importlib.util
from pathlib import Path
import sys
import re
import subprocess

import esphome.config_validation as cv
from esphome.core import CORE
import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esphome/components/voip_stack"


@pytest.fixture
def native(monkeypatch):
    CORE.reset()
    spec = importlib.util.spec_from_file_location("voip_queue_schema", COMPONENT / "__init__.py")
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, spec.name, module)
    spec.loader.exec_module(module)
    yield module
    CORE.reset()


def test_queues_share_group_bounds_and_optional_ha_text(native):
    assert native._validate_group_list(" Support, 700 ") == "Support, 700"
    for value in ("a" * 33, "a|b", "a\nb", ",".join(["x" * 32] * 8)):
        with pytest.raises(cv.Invalid):
            native._validate_group_list(value)
    text = importlib.import_module(native.__name__ + ".text")
    ha = importlib.import_module(native.__name__ + ".ha_integration")
    assert text.CONFIG_SCHEMA({"type": "queues", "name": "VoIP Queues"})["type"] == "queues"
    CORE.reset()
    assert ha.schema({})["queues"]["type"] == "queues"
    CORE.reset()
    assert ha.schema({"queues": False})["queues"] is False
    assert ha.schema(False) is False


def test_queue_setter_only_publishes_and_saves_existing_routing_owner():
    source = (COMPONENT / "voip_stack.cpp").read_text()
    setter = source.split("void VoipStack::set_queues(", 1)[1].split("\n#endif\n\nvoid", 1)[0]
    assert "normalize_group_list(queues)" in setter
    assert "publish_state(normalized)" in setter
    assert "schedule_save_routing_settings_()" in setter
    assert "register_account" not in setter and "transport_" not in setter
    header = (COMPONENT / "voip_stack.h").read_text()
    assert 'ROUTING_SETTINGS_VERSION = 1' in header
    assert '#ifdef USE_ESPHOME_VOIP_STACK_QUEUES' in header
    settings = (COMPONENT / "voip_settings.cpp").read_text()
    assert 'fnv1_hash("voip_stack_queues_settings")' in settings
    assert 'fnv1_hash("voip_stack_routing_settings")' in settings


def test_paging_auto_answer_requires_explicit_account_consent(native):
    registration = importlib.import_module(native.__name__ + ".registration")
    account = {"account_id": "pbx", "registrar": "192.0.2.1", "username": "desk", "password": "test"}
    assert registration.ACCOUNT_SCHEMA(account)["allow_paging_auto_answer"] is False
    assert registration.ACCOUNT_SCHEMA({**account, "allow_paging_auto_answer": True})["allow_paging_auto_answer"] is True


@pytest.mark.parametrize("paging", [False, True])
def test_paging_setup_compiles_with_real_base_transport_contract(tmp_path, paging):
    """Compile the actual setup call through its declared transport owner type."""
    source = (COMPONENT / "voip_stack.cpp").read_text()
    stack_header = (COMPONENT / "voip_stack.h").read_text()
    transport_header = (COMPONENT / "sip_transport.h").read_text()
    setup = source.split("bool VoipStack::setup_transport_()", 1)[1]
    gate = "#ifdef USE_ESPHOME_VOIP_PAGING\n" + setup.split("#ifdef USE_ESPHOME_VOIP_PAGING", 1)[1].split("#endif", 1)[0] + "\n#endif"
    owner = re.search(r"std::unique_ptr<SipPhoneTransport>\s+transport_;", stack_header).group()
    peers = re.search(r"std::vector<std::string>\s+paging_auto_answer_peers_;", stack_header).group()
    declaration = re.search(r"void set_paging_auto_answer_peers\([^;]+;", transport_header).group()
    cpp = tmp_path / "paging_setup_contract.cpp"
    cpp.write_text('''
#include <memory>
#include <vector>
#include "esphome/components/voip_stack/transport.h"
namespace esphome::voip_stack {
// Only the concrete setter declaration is needed; the base interface is real.
class SipTransport : public SipPhoneTransport { public:
#ifdef USE_ESPHOME_VOIP_PAGING
 DECLARATION
#endif
};
class VoipStack {
 OWNER
 PEERS
 void check_setup() {
 SETUP
 }
};
}
'''.replace("DECLARATION", declaration).replace("OWNER", owner).replace("PEERS", peers).replace("SETUP", gate))
    checked = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsyntax-only",
        "-DUSE_ESP32", *(["-DUSE_ESPHOME_VOIP_PAGING"] if paging else []), "-I", str(ROOT), str(cpp)],
        capture_output=True, text=True, timeout=20)
    assert checked.returncode == 0, checked.stderr
