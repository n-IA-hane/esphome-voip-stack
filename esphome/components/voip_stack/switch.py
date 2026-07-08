import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG
from esphome.core import CORE

from . import voip_stack_ns, VoipStack, CONF_VOIP_STACK_ID

DEPENDENCIES = ["voip_stack"]

# Switch types
CONF_ACTIVE = "active"
CONF_AUTO_ANSWER = "auto_answer"
CONF_DND = "dnd"
CONF_AEC = "aec"
CONF_CONFERENCE_RING = "conference_ring"

# C++ classes (simple - parent syncs state after boot)
VoipStackSwitch = voip_stack_ns.class_(
    "VoipStackSwitch", switch.Switch, cg.Parented.template(VoipStack)
)
VoipStackAutoAnswer = voip_stack_ns.class_(
    "VoipStackAutoAnswer", switch.Switch, cg.Parented.template(VoipStack)
)
VoipStackDndSwitch = voip_stack_ns.class_(
    "VoipStackDndSwitch", switch.Switch, cg.Parented.template(VoipStack)
)
VoipStackConferenceRingSwitch = voip_stack_ns.class_(
    "VoipStackConferenceRingSwitch", switch.Switch, cg.Parented.template(VoipStack)
)


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

def _switch_schema(switch_class, icon, entity_category=None):
    """Create switch schema for a specific switch type."""
    kwargs = {"icon": icon}
    if entity_category is not None:
        kwargs["entity_category"] = entity_category
    return switch.switch_schema(
        switch_class,
        **kwargs,
    ).extend(
        {
            cv.GenerateID(CONF_VOIP_STACK_ID): cv.use_id(VoipStack),
        }
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_VOIP_STACK_ID): cv.use_id(VoipStack),
        # On/off control for VoIP calls
        cv.Optional(CONF_ACTIVE): _switch_schema(VoipStackSwitch, "mdi:phone"),
        # Auto-answer incoming calls (default ON)
        cv.Optional(CONF_AUTO_ANSWER): _switch_schema(
            VoipStackAutoAnswer, "mdi:phone-in-talk", entity_category=ENTITY_CATEGORY_CONFIG
        ),
        # Do-not-disturb: reject incoming SIP INVITE with 486 Busy Here.
        cv.Optional(CONF_DND): _switch_schema(
            VoipStackDndSwitch, "mdi:minus-circle", entity_category=ENTITY_CATEGORY_CONFIG
        ),
        cv.Optional(CONF_CONFERENCE_RING): _switch_schema(
            VoipStackConferenceRingSwitch, "mdi:account-voice", entity_category=ENTITY_CATEGORY_CONFIG
        ),
        cv.Optional(CONF_AEC): cv.invalid(
            "voip_stack AEC switch was removed with standalone VoIP AEC. "
            "Use esp_audio_stack/esp_afe/esp_aec controls for software AEC."
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(_resolve_parent_id(config))

    if CONF_ACTIVE in config:
        conf = config[CONF_ACTIVE]
        var = await switch.new_switch(conf)
        cg.add(var.set_parent(parent))

    if CONF_AUTO_ANSWER in config:
        conf = config[CONF_AUTO_ANSWER]
        var = await switch.new_switch(conf)
        cg.add(var.set_parent(parent))
        # Register with parent for state sync after boot
        cg.add(parent.register_auto_answer_switch(var))

    if CONF_DND in config:
        conf = config[CONF_DND]
        var = await switch.new_switch(conf)
        cg.add(var.set_parent(parent))
        cg.add(parent.register_dnd_switch(var))

    if CONF_CONFERENCE_RING in config:
        conf = config[CONF_CONFERENCE_RING]
        var = await switch.new_switch(conf)
        cg.add(var.set_parent(parent))
        cg.add(parent.register_conference_ring_switch(var))
