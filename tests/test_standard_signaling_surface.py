"""Packaging contract: standard SIP media has no HA route-update side channel."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_media_route_side_channel_is_removed():
    source = ROOT / "esphome/components/voip_stack"
    for name in ("ha_integration.h", "ha_integration.py", "actions.h", "__init__.py", "voip_stack.h", "voip_fsm.cpp"):
        assert "set_media_route" not in (source / name).read_text()
    assert 'TYPE_MEDIA_ROUTE' not in (source / "text_sensor.py").read_text()
    diagnostics = (source / "voip_diagnostics.cpp").read_text()
    assert 'format("negotiated TX"' in diagnostics
    assert 'format("negotiated RX"' in diagnostics
