"""Native IMAP voicemail and public ESPHome MediaSource playback."""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome import final_validate as fv
from esphome.components import audio, esp32, media_source
from esphome.core import CORE
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["phone_client", "media_source", "audio", "ring_buffer"]
ns = cg.esphome_ns.namespace("imap_voicemail")
ImapVoicemail = ns.class_("ImapVoicemail", cg.Component, media_source.MediaSource)
VoipStack = cg.esphome_ns.namespace("voip_stack").class_("VoipStack", cg.Component)
RefreshAction = ns.class_("RefreshAction", automation.Action)
ControlAction = ns.class_("ControlAction", automation.Action)
ControlKind = ns.enum("ControlKind", is_class=True)


def bounded(value, maximum=1024):
    value = cv.string_strict(value)
    if len(value.encode()) > maximum or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise cv.Invalid(f"Value must contain at most {maximum} UTF-8 bytes and no control characters")
    return value


def host(value):
    value = bounded(value, 253)
    if value and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", value):
        raise cv.Invalid("Use an IPv4 address or DNS hostname; configure port separately")
    return value


def validate_config(config):
    external = bool(config["username"] or config["password"])
    if external and not (config["username"] and config["password"]):
        raise cv.Invalid("Explicit IMAP credentials require both username and password")
    if external and config.get("account_id"):
        raise cv.Invalid("Choose account_id or explicit IMAP credentials; account credentials are read-only")
    if config.get("voip_stack_id") is not None and not external and "account_id" not in config:
        config["account_id"] = "ha"
    if config.get("account_id") and "voip_stack_id" not in config:
        raise cv.Invalid("account_id requires voip_stack_id; standalone IMAP can use explicit credentials")
    if config["managed_by_ha"]:
        if config.get("account_id") != "ha":
            raise cv.Invalid("managed_by_ha requires account_id: ha")
        config["api_actions"] = True
    if config["tls"]:
        config["tls_support"] = True
    if config["api_actions"]:
        api = (CORE.raw_config or {}).get("api") or {}
        if not api.get("custom_services", False):
            raise cv.Invalid("Native IMAP actions require api: custom_services: true")
    return config


CONFIG_SCHEMA = cv.All(cv.require_esphome_version(2026, 10, 0), media_source.media_source_schema(ImapVoicemail).extend({
    cv.Optional("voip_stack_id"): cv.use_id(VoipStack),
    cv.Optional("account_id"): lambda v: bounded(v, 32),
    cv.Optional("host", default=""): host,
    cv.Optional("port", default=143): cv.port,
    cv.Optional("folder", default="INBOX"): lambda v: cv.All(cv.Length(min=1), lambda x: bounded(x, 256))(v),
    cv.Optional("username", default=""): bounded,
    cv.Optional("password", default=""): bounded,
    cv.Optional("tls", default=False): cv.boolean,
    cv.Optional("tls_support", default=False): cv.boolean,
    cv.Optional("ca_certificate", default=""): cv.string_strict,
    cv.Optional("managed_by_ha", default=False): cv.boolean,
    cv.Optional("api_actions", default=False): cv.boolean,
    cv.Optional("max_messages", default=16): cv.int_range(min=1, max=64),
    cv.Optional("metadata_buffer_size", default=16384): cv.int_range(min=4096, max=65536),
    cv.Optional("fetch_chunk_size", default=8192): cv.int_range(min=1024, max=32768),
    cv.Optional("audio_buffer_size", default=16384): cv.int_range(min=4096, max=131072),
    cv.Optional("decoder_output_buffer_size", default=8192): cv.int_range(min=2048, max=32768),
    cv.Optional("max_audio_bytes", default=16777216): cv.int_range(min=1024, max=268435456),
    cv.Optional("mark_read_on_play", default=True): cv.boolean,
    cv.Optional("timeout", default="5s"): cv.positive_time_period_milliseconds,
    cv.Optional("refresh_interval", default="5min"): cv.positive_time_period_milliseconds,
    cv.Optional("task_core", default=-1): cv.int_range(min=-1, max=1),
    cv.Optional("task_priority", default=2): cv.int_range(min=1, max=20),
    cv.Optional("task_stack_size", default=8192): cv.int_range(min=4096, max=32768),
    cv.Optional("task_stack_in_psram", default=False): cv.boolean,
    cv.Optional("fetch_task_core", default=-1): cv.int_range(min=-1, max=1),
    cv.Optional("fetch_task_priority", default=2): cv.int_range(min=1, max=20),
    cv.Optional("fetch_task_stack_size", default=8192): cv.int_range(min=4096, max=32768),
    cv.Optional("buffer_in_psram", default=True): cv.boolean,
    cv.Optional("codecs", default=["wav"]): cv.ensure_list(cv.one_of("wav", "mp3", "opus", "flac", lower=True)),
    cv.Optional("on_update"): automation.validate_automation(),
    cv.Optional("on_error"): automation.validate_automation(),
}).extend(cv.COMPONENT_SCHEMA), validate_config)


def final_validate(config):
    from esphome.components import socket
    socket.consume_sockets(2, "imap_voicemail")(config)
    if not config.get("account_id"):
        return config
    full = fv.full_config.get()
    path = full.get_path_for_id(config["voip_stack_id"])
    parent = full.get_config_for_path(path[:-1])
    ha = parent.get("ha_integration") or {}
    if config["managed_by_ha"] and not (isinstance(ha, dict) and ha.get("auto_register")):
        raise cv.Invalid("managed_by_ha requires voip_stack.ha_integration.auto_register: true")
    if "registration" not in parent and not (isinstance(ha, dict) and ha.get("auto_register")):
        raise cv.Invalid("IMAP account_id requires voip_stack registration or automatic HA registration")
    return config


FINAL_VALIDATE_SCHEMA = final_validate


async def to_code(config):
    if config["tls_support"]:
        cg.add_define("USE_PHONE_CLIENT_TLS")
        esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    if config["task_stack_in_psram"]:
        esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY", True)
    if config["api_actions"]:
        cg.add_define("USE_IMAP_VOICEMAIL_API")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)
    if "voip_stack_id" in config:
        cg.add_define("USE_IMAP_VOICEMAIL_ACCOUNT")
        cg.add(var.set_parent(await cg.get_variable(config["voip_stack_id"])))
    cg.add(var.set_account_id(config.get("account_id", "")))
    cg.add(var.set_managed_by_ha(config["managed_by_ha"]))
    cg.add(var.set_server(config["host"], config["port"], config["tls"]))
    cg.add(var.set_folder(config["folder"]))
    cg.add(var.set_credentials(config["username"], config["password"]))
    cg.add(var.set_ca_certificate(config["ca_certificate"]))
    cg.add(var.set_timeout(config["timeout"].total_milliseconds))
    cg.add(var.set_refresh_interval(config["refresh_interval"].total_milliseconds))
    for key in ("max_messages", "metadata_buffer_size", "fetch_chunk_size", "audio_buffer_size", "decoder_output_buffer_size", "max_audio_bytes",
                "mark_read_on_play", "task_core", "task_priority", "task_stack_size", "task_stack_in_psram", "fetch_task_core",
                "fetch_task_priority", "fetch_task_stack_size", "buffer_in_psram"):
        cg.add(getattr(var, f"set_{key}")(config[key]))
    for codec in config["codecs"]:
        getattr(audio, f"request_{codec}_support")()
    for conf in config.get("on_update", []):
        await automation.build_automation(var.get_update_trigger(), [(cg.uint32, "total"), (cg.uint32, "count"), (cg.uint32, "uid_validity")], conf)
    for conf in config.get("on_error", []):
        await automation.build_automation(var.get_error_trigger(), [(cg.std_string, "reason")], conf)


@automation.register_action("imap_voicemail.refresh", RefreshAction, cv.Schema({cv.GenerateID(): cv.use_id(ImapVoicemail)}), synchronous=True)
async def refresh_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


def register_control(name, kind, *, read=False, page=False):
    fields = {cv.GenerateID(): cv.use_id(ImapVoicemail)}
    fields[cv.Required("page" if page else "uid")] = cv.templatable(cv.uint32_t)
    fields[cv.Optional("uid_validity", default=0) if kind == "PLAY" or page else cv.Required("uid_validity")] = cv.templatable(cv.uint32_t)
    if read:
        fields[cv.Optional("read", default=True)] = cv.templatable(cv.boolean)

    @automation.register_action(name, ControlAction, cv.Schema(fields), synchronous=True)
    async def control(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_kind(getattr(ControlKind, kind)))
        cg.add(var.set_uid(await cg.templatable(config["page" if page else "uid"], args, cg.uint32)))
        cg.add(var.set_uid_validity(await cg.templatable(config["uid_validity"], args, cg.uint32)))
        cg.add(var.set_read(await cg.templatable(config.get("read", True), args, cg.bool_)))
        return var


register_control("imap_voicemail.play", "PLAY")
register_control("imap_voicemail.mark_read", "READ", read=True)
register_control("imap_voicemail.delete", "DELETE")
register_control("imap_voicemail.page", "PAGE", page=True)
