import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    CONF_MIN_VALUE,
    CONF_MAX_VALUE,
    CONF_STEP,
    CONF_MODE,
)

from . import voip_stack_ns, VoipStack, CONF_VOIP_STACK_ID

DEPENDENCIES = ["voip_stack"]

# Number types
CONF_MASTER_VOLUME = "master_volume"
CONF_MIC_GAIN = "mic_gain"

# C++ classes (simple - parent syncs state after boot)
VoipStackVolume = voip_stack_ns.class_(
    "VoipStackVolume", number.Number, cg.Parented.template(VoipStack)
)
VoipStackMicGain = voip_stack_ns.class_(
    "VoipStackMicGain", number.Number, cg.Parented.template(VoipStack)
)


def _number_schema(number_class, icon, min_val, max_val, step):
    """Create number schema for a specific number type."""
    return number.number_schema(
        number_class,
        icon=icon,
    ).extend(
        {
            cv.GenerateID(CONF_VOIP_STACK_ID): cv.use_id(VoipStack),
            cv.Optional(CONF_MIN_VALUE, default=min_val): cv.float_,
            cv.Optional(CONF_MAX_VALUE, default=max_val): cv.float_,
            cv.Optional(CONF_STEP, default=step): cv.float_,
            cv.Optional(CONF_MODE, default="SLIDER"): cv.enum(
                number.NUMBER_MODES, upper=True
            ),
        }
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VOIP_STACK_ID): cv.use_id(VoipStack),
        # Master volume (0-100%), default 100%
        cv.Optional(CONF_MASTER_VOLUME): _number_schema(
            VoipStackVolume, "mdi:volume-high", 0, 100, 5
        ),
        # Mic gain in dB (-20 to +20), default 0dB
        cv.Optional(CONF_MIC_GAIN): _number_schema(
            VoipStackMicGain, "mdi:microphone", -20, 20, 1
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VOIP_STACK_ID])

    if CONF_MASTER_VOLUME in config:
        conf = config[CONF_MASTER_VOLUME]
        var = await number.new_number(
            conf,
            min_value=conf[CONF_MIN_VALUE],
            max_value=conf[CONF_MAX_VALUE],
            step=conf[CONF_STEP],
        )
        cg.add(var.set_parent(parent))
        # Register with parent for state sync after boot
        cg.add(parent.register_volume_number(var))

    if CONF_MIC_GAIN in config:
        conf = config[CONF_MIC_GAIN]
        var = await number.new_number(
            conf,
            min_value=conf[CONF_MIN_VALUE],
            max_value=conf[CONF_MAX_VALUE],
            step=conf[CONF_STEP],
        )
        cg.add(var.set_parent(parent))
        # Register with parent for state sync after boot
        cg.add(parent.register_mic_gain_number(var))
