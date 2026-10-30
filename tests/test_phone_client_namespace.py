"""Compile the real transport with full-firmware component namespaces present."""

from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_posix_transport_does_not_resolve_global_functions_as_component_namespaces(tmp_path):
    preamble = tmp_path / "component_namespaces.h"
    preamble.write_text("namespace esphome { namespace select {} namespace socket {} }\n")
    result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                             "-include", str(preamble), "-fsyntax-only",
                             str(ROOT / "esphome/components/phone_client/net_stream.cpp")],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr


def test_custom_api_action_parameters_use_esphome_supported_scalar_types():
    # ESPHome specializes int32_t, not C++ int. They are different types on
    # Xtensa even though the host ABI aliases them, so host linking alone
    # cannot protect this native-action boundary.
    supported = {"std::string", "int32_t", "float", "bool"}
    for component, owner in (("ldap_directory", "LdapDirectory"), ("imap_voicemail", "ImapVoicemail")):
        folder = ROOT / "esphome/components" / component
        implementation = (folder / f"{component}.cpp").read_text()
        declaration = (folder / f"{component}.h").read_text()
        callbacks = re.findall(rf"register_service\(&{owner}::(\w+)", implementation)
        assert callbacks
        for callback in callbacks:
            signature = re.search(rf"\bvoid\s+{callback}\s*\(([^)]*)\)", declaration)
            assert signature, callback
            arguments = signature[1].split(",") if signature[1].strip() else []
            types = {argument.strip().rsplit(" ", 1)[0] for argument in arguments}
            assert types <= supported, (owner, callback, types - supported)
