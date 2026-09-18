"""Native HA surface for a VoIP endpoint, independent of audio and UI profiles."""
from importlib import import_module

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.core import CORE
from esphome.const import CONF_ID, CONF_NAME

CONF_HA_INTEGRATION = "ha_integration"
LEGACY_PACKAGE = "legacy_ha_package"
ACTIONS = {
    "start_call", "answer_call", "decline_call", "hangup_call", "set_media_route",
    "set_ha_peer_name", "set_roster_json", "set_contacts", "add_contact",
    "remove_contact", "flush_contacts", "update_contacts",
}
ROLES = ("endpoint", "state", "media_route", "caller", "destination", "last_reason", "contacts")
TEXT_ROLES = ("extension", "ring_groups", "conference_groups")


def enabled(config):
    value = config.get(CONF_HA_INTEGRATION)
    if value is False:
        return False
    return value is not None or "api" in (CORE.raw_config or {})


def schema(value):
    sensors, texts, switches = (import_module(f".{name}", __package__) for name in ("text_sensor", "text", "switch"))
    if value is False:
        return False
    if value is True:
        value = {}
    fields = {}
    for role in ROLES:
        fields[cv.Optional(role, default={})] = lambda conf, role=role: sensors.CONFIG_SCHEMA({
            CONF_ID: f"voip_{role}", CONF_NAME: sensors.TYPES[role][0], "type": role, **conf,
        })
    for role in TEXT_ROLES:
        fields[cv.Optional(role, default={})] = lambda conf, role=role: texts.CONFIG_SCHEMA({
            CONF_ID: f"voip_{role}", CONF_NAME: texts.TYPES[role][1], "type": role, **conf,
        })
    fields[cv.Optional("conference_ring", default={})] = lambda conf: switches.CONFIG_SCHEMA({
        "conference_ring": {CONF_ID: "voip_conference_ring", CONF_NAME: "VoIP Ring On Conference",
                            "restore_mode": "RESTORE_DEFAULT_OFF", **conf},
    })
    return cv.Schema(fields)(value)


def prepare(config):
    config = dict(config)
    if CONF_HA_INTEGRATION not in config:
        config[CONF_HA_INTEGRATION] = {} if "api" in (CORE.raw_config or {}) else False
    return config


def validate(config, full):
    if config[CONF_HA_INTEGRATION] is False:
        return
    if "api" not in full:
        raise cv.Invalid("voip_stack.ha_integration requires api:; use ha_integration: false for standalone SIP")
    if not full["api"].get("custom_services", False):
        raise cv.Invalid("Native VoIP actions require api: custom_services: true. No VoIP HA package is needed.")
    for action in full["api"].get("actions", []):
        name = action.get("action", action.get("service", ""))
        if name in ACTIONS:
            raise cv.Invalid(f"API action '{name}' is now provided by voip_stack. Remove the old VoIP HA package or copied action.")
    for item in full.get("text_sensor", []):
        if item.get("platform") == "voip_stack" and item.get("type") in ROLES:
            raise cv.Invalid(f"VoIP {item['type']} is now managed by ha_integration. Remove the old HA package; move entity customizations under voip_stack.ha_integration.")
    for item in full.get("text", []):
        if item.get("platform") == "voip_stack" and item.get("type") in TEXT_ROLES:
            raise cv.Invalid("VoIP routing text entities are now managed by ha_integration; remove the old HA package")
    for item in full.get("switch", []):
        if item.get("platform") == "voip_stack" and "conference_ring" in item:
            raise cv.Invalid("VoIP conference_ring is now managed by ha_integration; remove the old HA package")


async def to_code(parent, config):
    if config[CONF_HA_INTEGRATION] is False:
        return
    sensors, texts, switches = (import_module(f".{name}", __package__) for name in ("text_sensor", "text", "switch"))
    cg.add_define("USE_VOIP_HA_INTEGRATION")
    adapter = cg.new_Pvariable(config["ha_adapter_id"])
    cg.add(adapter.set_parent(parent))
    await cg.register_component(adapter, {})
    for role in ROLES:
        await sensors.to_code(config[CONF_HA_INTEGRATION][role])
    for role in TEXT_ROLES:
        await texts.to_code(config[CONF_HA_INTEGRATION][role])
    await switches.to_code(config[CONF_HA_INTEGRATION]["conference_ring"])
