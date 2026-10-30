"""Verify component codegen re-enables its required IDF dependencies."""

import asyncio
import importlib.util
import sys
from pathlib import Path
from unittest.mock import AsyncMock, Mock
import pytest

from esphome.components import esp32
from esphome.core import CORE


@pytest.mark.parametrize("legacy", [False, True])
def test_legacy_roster_owns_its_cjson_dependency(monkeypatch, legacy):
    path = Path(__file__).resolve().parents[1] / "esphome/components/voip_stack/__init__.py"
    spec = importlib.util.spec_from_file_location("voip_dependency_test", path)
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, spec.name, module)
    spec.loader.exec_module(module)

    monkeypatch.setattr(module.ha_integration, "to_code", AsyncMock())

    excluded = {"json", "console"}
    monkeypatch.setitem(CORE.data, esp32.KEY_ESP32, {esp32.KEY_EXCLUDE_COMPONENTS: excluded})
    monkeypatch.setattr(module.cg, "new_Pvariable", Mock(return_value=object()))
    monkeypatch.setattr(module.cg, "register_component", AsyncMock())
    monkeypatch.setattr(module.cg, "add_define", Mock())
    for name in (
        "_add_core_settings",
        "_add_device_and_audio_settings",
        "_build_voip_automations",
        "_bind_ha_phonebook_sensor",
    ):
        monkeypatch.setattr(module, name, AsyncMock())
    for name in ("_add_transport_settings", "_add_static_contacts"):
        monkeypatch.setattr(module, name, Mock())

    asyncio.run(module.to_code({module.CONF_ID: "phone", "ha_integration": {"legacy_roster": legacy}}))

    assert excluded == ({"console"} if legacy else {"json", "console"})
