"""Optional bounded RFC4235 watches on the existing SIP worker."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.core import CORE
from .registration import account_id

SCHEMA = cv.Schema({
    cv.Optional("account_id", default="ha"): account_id,
    cv.Optional("max_subscriptions", default=16): cv.int_range(min=1, max=64),
    cv.Optional("watch_contacts", default=True): cv.boolean,
    cv.Optional("presence", default=False): cv.boolean,
    cv.Optional("pickup_prefix", default=""): cv.string_strict,
    cv.Optional("buffers_in_psram"): cv.boolean,
})


def final_validate(config, full):
    if "blf" not in config:
        if "on_blf_state" in config:
            raise cv.Invalid("on_blf_state requires voip_stack.blf")
        return
    if "registration" not in config:
        raise cv.Invalid("BLF requires a configured SIP registration account")
    conf = config["blf"]
    if conf.get("buffers_in_psram", "psram" in full) and "psram" not in full:
        raise cv.Invalid("BLF buffers_in_psram requires psram:")
    prefix = conf["pickup_prefix"]
    if len(prefix) > 32 or any(ord(c) < 33 or ord(c) == 127 or c in '<>|,;"' for c in prefix):
        raise cv.Invalid("pickup_prefix must contain at most 32 visible SIP dial characters")


async def to_code(parent, config):
    if "blf" not in config:
        return
    conf = config["blf"]
    cg.add_define("USE_ESPHOME_VOIP_BLF")
    if conf["presence"]:
        cg.add_define("USE_ESPHOME_VOIP_PRESENCE")
    if conf.get("buffers_in_psram", "psram" in (CORE.config or {})):
        cg.add_define("USE_ESPHOME_VOIP_BLF_PSRAM")
    cg.add(parent.set_blf_configuration(conf["account_id"], conf["max_subscriptions"],
                                       conf["pickup_prefix"], conf["watch_contacts"]))
    for trigger in config.get("on_blf_state", []):
        await automation.build_automation(parent.get_blf_state_trigger(), [
            (cg.std_string, "contact_id"), (cg.std_string, "account_id"),
            (cg.std_string, "state"), (cg.bool_, "available"),
        ], trigger)
