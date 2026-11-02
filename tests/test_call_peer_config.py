"""Peer updates stay optional and separate from call lifecycle automations."""
from test_sip_registration_config import run_config


def test_peer_callback_codegen_is_real_and_optional(tmp_path):
    result = run_config(tmp_path, "  on_peer_changed:\n    then:\n      - logger.log:\n          format: 'Peer: %s'\n          args: [peer.c_str()]\n")
    assert result.returncode == 0, result.stdout + result.stderr
    generated = next(tmp_path.rglob("main.cpp")).read_text()
    defines = next(tmp_path.rglob("defines.h")).read_text()
    assert "#define USE_ESPHOME_VOIP_PEER_EVENTS" in defines
    assert "get_peer_changed_trigger()" in generated
    assert "get_in_call_trigger()" not in generated


def test_peer_callback_off_compiles_out_the_trigger(tmp_path):
    result = run_config(tmp_path, "")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "USE_ESPHOME_VOIP_PEER_EVENTS" not in next(tmp_path.rglob("defines.h")).read_text()
    assert "get_peer_changed_trigger" not in next(tmp_path.rglob("main.cpp")).read_text()
