"""Native HA surface for a VoIP endpoint, independent of audio and UI profiles."""
from importlib import import_module

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.core import CORE
from esphome.const import CONF_ID, CONF_NAME

CONF_HA_INTEGRATION = "ha_integration"
LEGACY_PACKAGE = "legacy_ha_package"
ACTIONS = {
    "start_call", "start_call_via_ha", "hangup_call_if_id", "answer_call", "decline_call", "hangup_call", "set_media_route",
    "set_ha_peer_name", "set_contacts", "add_contact", "send_dtmf",
    "remove_contact", "flush_contacts", "update_contacts",
}
ROLES = ("endpoint", "state", "media_route", "caller", "destination", "last_reason", "contacts")
TEXT_ROLES = ("extension", "ring_groups", "conference_groups")


def enabled(config):
    value = config.get(CONF_HA_INTEGRATION)
    if value is False:
        return False
    return value is not None or "api" in (CORE.raw_config or {})


def legacy_roster_enabled(config):
    value = config.get(CONF_HA_INTEGRATION)
    return isinstance(value, dict) and value.get("legacy_roster", not value.get("auto_register", False))


def schema(value):
    sensors, texts, switches = (import_module(f".{name}", __package__) for name in ("text_sensor", "text", "switch"))
    if value is False:
        return False
    if value is True:
        value = {}
    fields = {}
    fields[cv.Optional("message_waiting")] = cv.invalid("Native API mailbox updates are retired. Use registration.message_waiting for standard SIP MWI.")
    fields[cv.Optional("auto_register", default=False)] = cv.boolean
    fields[cv.Optional("legacy_roster", default=not (isinstance(value, dict) and value.get("auto_register", False)))] = cv.boolean
    for role in ROLES:
        if role == "endpoint":
            fields[cv.Optional(role, default=False)] = lambda conf: False if conf is False else sensors.CONFIG_SCHEMA({
                CONF_ID: "voip_endpoint", CONF_NAME: sensors.TYPES["endpoint"][0], "type": "endpoint",
                **({} if conf is True else conf),
            })
            continue
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
    ha = config.get(CONF_HA_INTEGRATION)
    if isinstance(ha, dict) and ha.get("auto_register", False):
        registration = dict(config.get("registration") or {})
        registration.setdefault("default_line", "ha")
        config["registration"] = registration
        if "blf" in config:
            blf = dict(config["blf"])
            blf.setdefault("pickup_prefix", "pickup.")
            config["blf"] = blf
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
        registration_actions = {"set_account", "remove_account", "reset_account", "register_account", "unregister_account", "select_line", "start_call_on_line"} if "registration" in config else set()
        managed_actions = {"set_ha_display_name"} if config[CONF_HA_INTEGRATION].get("auto_register") else set()
        roster_actions = {"set_roster_json"} if legacy_roster_enabled(config) else set()
        if name in ACTIONS | registration_actions | managed_actions | roster_actions:
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
    if config[CONF_HA_INTEGRATION]["auto_register"]:
        cg.add_define("USE_VOIP_HA_AUTO_REGISTER")
    for role in ROLES:
        if config[CONF_HA_INTEGRATION][role] is False:
            continue
        await sensors.to_code(config[CONF_HA_INTEGRATION][role])
    for role in TEXT_ROLES:
        await texts.to_code(config[CONF_HA_INTEGRATION][role])
    await switches.to_code(config[CONF_HA_INTEGRATION]["conference_ring"])
