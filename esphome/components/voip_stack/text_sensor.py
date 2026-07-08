import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ICON, CONF_NAME

from . import CONF_VOIP_STACK_ID, VoipStack, resolve_parent_id

DEPENDENCIES = ["voip_stack"]

CONF_TYPE = "type"

TYPE_ENDPOINT = "endpoint"
TYPE_STATE = "state"
TYPE_TRANSPORT = "transport"
TYPE_LAST_REASON = "last_reason"
TYPE_SIP_SNAPSHOT = "sip_snapshot"
TYPE_DESTINATION = "destination"
TYPE_CALLER = "caller"
TYPE_CONTACTS = "contacts"

TYPES = {
    TYPE_ENDPOINT: ("VoIP Endpoint", "mdi:lan-connect", "set_endpoint_sensor"),
    TYPE_STATE: ("VoIP State", "mdi:phone-settings", "set_state_sensor"),
    TYPE_TRANSPORT: ("VoIP Transport", "mdi:swap-horizontal", "set_transport_sensor"),
    TYPE_LAST_REASON: ("VoIP Last Reason", "mdi:phone-alert", "set_last_reason_sensor"),
    TYPE_SIP_SNAPSHOT: ("VoIP SIP Snapshot", "mdi:phone-log", "set_sip_snapshot_sensor"),
    TYPE_DESTINATION: ("VoIP Destination", "mdi:phone-forward", "set_destination_sensor"),
    TYPE_CALLER: ("VoIP Caller", "mdi:phone-incoming", "set_caller_sensor"),
    TYPE_CONTACTS: ("VoIP Contacts", "mdi:account-group", "set_contacts_sensor"),
}


def _apply_type_defaults(config):
    config = dict(config)
    default_name, default_icon, _ = TYPES[config[CONF_TYPE]]
    config.setdefault(CONF_NAME, default_name)
    config.setdefault(CONF_ICON, default_icon)
    return config


CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema().extend(
        {
            cv.Required(CONF_TYPE): cv.one_of(*TYPES, lower=True),
            cv.Optional(CONF_VOIP_STACK_ID): cv.use_id(VoipStack),
        }
    ),
    _apply_type_defaults,
)


async def to_code(config):
    parent = await cg.get_variable(resolve_parent_id(config))
    var = await text_sensor.new_text_sensor(config)
    setter = getattr(parent, TYPES[config[CONF_TYPE]][2])
    cg.add(setter(var))
