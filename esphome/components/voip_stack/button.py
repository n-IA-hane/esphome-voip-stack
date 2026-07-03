import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import voip_stack_ns, VoipStack, CONF_VOIP_STACK_ID

DEPENDENCIES = ["voip_stack"]

CONF_CALL = "call"
CONF_NEXT_CONTACT = "next_contact"
CONF_PREVIOUS_CONTACT = "previous_contact"
CONF_DECLINE = "decline"

VoipCallButton = voip_stack_ns.class_(
    "VoipCallButton", button.Button, cg.Parented.template(VoipStack)
)
VoipNextContactButton = voip_stack_ns.class_(
    "VoipNextContactButton", button.Button, cg.Parented.template(VoipStack)
)
VoipPreviousContactButton = voip_stack_ns.class_(
    "VoipPreviousContactButton", button.Button, cg.Parented.template(VoipStack)
)
VoipDeclineButton = voip_stack_ns.class_(
    "VoipDeclineButton", button.Button, cg.Parented.template(VoipStack)
)


def _button_schema(button_class, icon):
    return button.button_schema(button_class, icon=icon)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VOIP_STACK_ID): cv.use_id(VoipStack),
        cv.Optional(CONF_CALL): _button_schema(VoipCallButton, "mdi:phone"),
        cv.Optional(CONF_NEXT_CONTACT): _button_schema(
            VoipNextContactButton, "mdi:skip-next"
        ),
        cv.Optional(CONF_PREVIOUS_CONTACT): _button_schema(
            VoipPreviousContactButton, "mdi:skip-previous"
        ),
        cv.Optional(CONF_DECLINE): _button_schema(
            VoipDeclineButton, "mdi:phone-hangup"
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VOIP_STACK_ID])

    for key in (CONF_CALL, CONF_NEXT_CONTACT, CONF_PREVIOUS_CONTACT, CONF_DECLINE):
        if key not in config:
            continue
        var = await button.new_button(config[key])
        cg.add(var.set_parent(parent))
