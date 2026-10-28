"""Bounded software JPEG renderer, optionally connected to VoIP."""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S3,
    add_idf_component,
    add_idf_sdkconfig_option,
    only_on_variant,
)
from esphome.components.lvgl.defines import add_lv_use
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@n-IA-hane"]
DEPENDENCIES = ["esp32", "lvgl"]

CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_FRAMERATE = "framerate"
CONF_MAX_JPEG_BYTES = "max_jpeg_bytes"
CONF_VOIP = "voip"
CONF_TASK_CORE = "task_core"
CONF_TASK_PRIORITY = "task_priority"
CONF_TASK_STACK_SIZE = "task_stack_size"
CONF_TASK_STACK_IN_PSRAM = "task_stack_in_psram"
CONF_BUFFER_IN_PSRAM = "buffer_in_psram"
CONF_ON_FIRST_FRAME = "on_first_frame"
CONF_ON_VIDEO_ENDED = "on_video_ended"

ns = cg.esphome_ns.namespace("esp_video_renderer")
EspVideoRenderer = ns.class_("EspVideoRenderer", cg.Component)
# Declaring the optional interface type does not load the SIP component.
EncodedVideoSink = cg.esphome_ns.namespace("voip_stack").class_("EncodedVideoSink")
EspVideoRendererSink = ns.class_("EspVideoRendererSink", EncodedVideoSink)
StartAction = ns.class_("StartAction", automation.Action)
StopAction = ns.class_("StopAction", automation.Action)


def _task_core(value):
    if isinstance(value, str) and value.lower() == "any":
        return "any"
    return cv.int_range(min=0, max=1)(value)


def _stack_size(value):
    size = cv.int_range(min=4096, max=65536)(cv.validate_bytes(value))
    if size % 16:
        raise cv.Invalid("task_stack_size must be a multiple of 16 bytes")
    return size


def _dimensions(config):
    for key in (CONF_WIDTH, CONF_HEIGHT):
        if config[key] % 8:
            raise cv.Invalid(f"{key} must be a multiple of 8")
    if config[CONF_WIDTH] * config[CONF_HEIGHT] > 640 * 480:
        raise cv.Invalid("software renderer supports at most 640x480 pixels")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EspVideoRenderer),
            cv.Optional(CONF_WIDTH, default=320): cv.int_range(min=8, max=640),
            cv.Optional(CONF_HEIGHT, default=240): cv.int_range(min=8, max=640),
            cv.Optional(CONF_FRAMERATE, default=2): cv.int_range(min=1, max=15),
            cv.Optional(CONF_MAX_JPEG_BYTES, default=131072): cv.int_range(
                min=4096, max=524288
            ),
            cv.Optional(CONF_TASK_CORE, default="any"): _task_core,
            cv.Optional(CONF_TASK_PRIORITY, default=5): cv.int_range(min=1, max=24),
            cv.Optional(CONF_TASK_STACK_SIZE, default=8192): _stack_size,
            cv.Optional(CONF_TASK_STACK_IN_PSRAM, default=True): cv.boolean,
            cv.Optional(CONF_BUFFER_IN_PSRAM, default=True): cv.boolean,
            cv.Optional(CONF_VOIP): cv.Schema(
                {cv.GenerateID(): cv.declare_id(EspVideoRendererSink)}
            ),
            cv.Optional(CONF_ON_FIRST_FRAME): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_VIDEO_ENDED): automation.validate_automation(
                single=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _dimensions,
    cv.only_on_esp32,
    only_on_variant(supported=[VARIANT_ESP32S3, VARIANT_ESP32P4]),
)


def _final_validate(config):
    from esphome import final_validate as fv

    full = fv.full_config.get()
    if (config[CONF_TASK_STACK_IN_PSRAM] or config[CONF_BUFFER_IN_PSRAM]) and "psram" not in full:
        raise cv.Invalid("PSRAM placement requires the psram component; choose internal placement explicitly otherwise")
    if CONF_VOIP in config:
        phone = full.get("voip_stack", {})
        if phone.get("video", {}).get("codec") != "jpeg":
            raise cv.Invalid("esp_video_renderer.voip requires voip_stack video codec jpeg")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    if not CORE.using_toolchain_esp_idf:
        raise cv.Invalid("esp_video_renderer requires the esp-idf framework")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for key in (CONF_WIDTH, CONF_HEIGHT, CONF_FRAMERATE, CONF_MAX_JPEG_BYTES,
                CONF_TASK_PRIORITY, CONF_TASK_STACK_SIZE,
                CONF_TASK_STACK_IN_PSRAM, CONF_BUFFER_IN_PSRAM):
        cg.add(getattr(var, f"set_{key}")(config[key]))
    cg.add(var.set_task_core(-1 if config[CONF_TASK_CORE] == "any" else config[CONF_TASK_CORE]))
    add_idf_component(name="espressif/esp_jpeg", ref="1.3.2")
    if config[CONF_TASK_STACK_IN_PSRAM]:
        add_idf_sdkconfig_option("CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM", True)
    add_lv_use("image", "label")
    if CONF_VOIP in config:
        cg.add_define("USE_ESP_VIDEO_RENDERER_VOIP")
        cg.new_Pvariable(config[CONF_VOIP][CONF_ID], var)
    for key, method in (
        (CONF_ON_FIRST_FRAME, "get_first_frame_trigger"),
        (CONF_ON_VIDEO_ENDED, "get_video_ended_trigger"),
    ):
        if key in config:
            await automation.build_automation(getattr(var, method)(), [], config[key])


@automation.register_action(
    "esp_video_renderer.start", StartAction,
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(EspVideoRenderer)}),
    synchronous=True,
)
@automation.register_action(
    "esp_video_renderer.stop", StopAction,
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(EspVideoRenderer)}),
    synchronous=True,
)
async def action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
