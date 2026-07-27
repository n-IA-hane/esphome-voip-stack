import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG
from esphome.core import CORE

from . import (
    CONF_VIDEO,
    CONF_VOIP_STACK_ID,
    VoipStack,
    resolve_parent_id,
    voip_stack_ns,
)

DEPENDENCIES = ["voip_stack"]

# Switch types
CONF_ACTIVE = "active"
CONF_AUTO_ANSWER = "auto_answer"
CONF_DND = "dnd"
CONF_AEC = "aec"
CONF_CONFERENCE_RING = "conference_ring"
CONF_VIDEO_SEND = "video_send"

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
VoipStackVideoSendSwitch = voip_stack_ns.class_(
    "VoipStackVideoSendSwitch", switch.Switch, cg.Parented.template(VoipStack)
)


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
        cv.Optional(CONF_VIDEO_SEND): _switch_schema(
            VoipStackVideoSendSwitch,
            "mdi:video",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
        cv.Optional(CONF_AEC): cv.invalid(
            "voip_stack AEC switch was removed with standalone VoIP AEC. "
            "Use esp_audio_stack/esp_afe/esp_aec controls for software AEC."
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(resolve_parent_id(config))

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

    if CONF_VIDEO_SEND in config:
        voip_config = (CORE.config or {}).get("voip_stack")
        if not isinstance(voip_config, dict) or CONF_VIDEO not in voip_config:
            raise cv.Invalid(
                "voip_stack video_send switch requires voip_stack.video."
            )
        conf = config[CONF_VIDEO_SEND]
        var = await switch.new_switch(conf)
        cg.add(var.set_parent(parent))
        cg.add(parent.register_video_send_switch(var))
