import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import text
from esphome.const import CONF_ICON, CONF_NAME
from esphome.core import CORE

from . import CONF_VOIP_STACK_ID, VoipStack, voip_stack_ns, _validate_group_list

DEPENDENCIES = ["voip_stack"]

CONF_TYPE = "type"

TYPE_RING_GROUPS = "ring_groups"
TYPE_CONFERENCE_GROUPS = "conference_groups"
TYPE_EXTENSION = "extension"

VoipStackGroupsText = voip_stack_ns.class_(
    "VoipStackGroupsText", text.Text, cg.Parented.template(VoipStack)
)

TYPES = {
    TYPE_EXTENSION: (
        2,
        "VoIP Extension",
        "mdi:phone-dial",
        "set_extension_text",
        "get_extension",
        32,
        r"^[^|,;\r\n]*$",
    ),
    TYPE_RING_GROUPS: (
        0,
        "VoIP Ring Groups",
        "mdi:phone-ring",
        "set_ring_groups_text",
        "get_ring_groups",
        240,
        r"^[^|;\r\n]*$",
    ),
    TYPE_CONFERENCE_GROUPS: (
        1,
        "VoIP Conference Groups",
        "mdi:account-voice",
        "set_conference_groups_text",
        "get_conference_groups",
        240,
        r"^[^|;\r\n]*$",
    ),
}


def _resolve_parent_id(config):
    if CONF_VOIP_STACK_ID in config:
        return config[CONF_VOIP_STACK_ID]
    try:
        full_config = fv.full_config.get()
    except LookupError:
        full_config = CORE.config
    voip_configs = full_config.get("voip_stack", [])
    if isinstance(voip_configs, dict):
        voip_configs = [voip_configs]
    if len(voip_configs) != 1:
        raise cv.Invalid("voip_stack_id is required when zero or multiple voip_stack components are configured")
    return voip_configs[0]["id"]


def _apply_type_defaults(config):
    config = dict(config)
    _, default_name, default_icon, _, _, _, _ = TYPES[config[CONF_TYPE]]
    config.setdefault(CONF_NAME, default_name)
    config.setdefault(CONF_ICON, default_icon)
    return config


CONFIG_SCHEMA = cv.All(
    text.text_schema(VoipStackGroupsText, mode="TEXT").extend(
        {
            cv.Required(CONF_TYPE): cv.one_of(*TYPES, lower=True),
            cv.Optional(CONF_VOIP_STACK_ID): cv.use_id(VoipStack),
        }
    ),
    _apply_type_defaults,
)


async def to_code(config):
    parent = await cg.get_variable(_resolve_parent_id(config))
    kind, _, _, setter_name, getter_name, max_length, pattern = TYPES[config[CONF_TYPE]]
    var = await text.new_text(config, max_length=max_length, pattern=pattern)
    cg.add(var.set_parent(parent))
    cg.add(var.set_kind(kind))
    cg.add(getattr(parent, setter_name)(var))
    cg.add(var.publish_state(getattr(parent, getter_name)()))
