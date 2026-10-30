"""LDAPv3 directory, committed atomically into the existing SIP phonebook."""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome import final_validate as fv
from esphome.components import esp32
from esphome.core import CORE
from esphome.const import CONF_ID

from esphome.components.voip_stack import VoipStack

DEPENDENCIES = ["esp32", "voip_stack"]
AUTO_LOAD = ["phone_client"]
ns = cg.esphome_ns.namespace("ldap_directory")
LdapDirectory = ns.class_("LdapDirectory", cg.Component)
RefreshAction = ns.class_("RefreshAction", automation.Action)


def bounded(value, limit=1024):
    value = cv.string_strict(value)
    if len(value.encode()) > limit or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise cv.Invalid(f"Value must contain at most {limit} UTF-8 bytes and no control characters")
    return value


def server(value):
    value = bounded(value, 253)
    if value and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", value):
        raise cv.Invalid("Use an IPv4 address or DNS hostname; configure port separately")
    return value


def validate_filter(value):
    value = bounded(value)
    if ":=" in value:
        raise cv.Invalid("Extensible-match LDAP filters are not supported")
    nodes = 0

    def one(position, depth):
        nonlocal nodes
        nodes += 1
        if depth > 8 or nodes > 64 or position >= len(value) or value[position] != "(":
            raise cv.Invalid("Invalid or overly complex LDAP filter")
        position += 1
        if position >= len(value):
            raise cv.Invalid("Unbalanced LDAP filter")
        operation = value[position]
        if operation in "&|!":
            position += 1
            children = 0
            while position < len(value) and value[position] == "(":
                position = one(position, depth + 1)
                children += 1
                if children > 32:
                    raise cv.Invalid("LDAP boolean filter has too many operands")
            if not children or (operation == "!" and children != 1):
                raise cv.Invalid("LDAP NOT needs one operand; AND/OR need at least one")
        else:
            match = re.match(r"[A-Za-z0-9][A-Za-z0-9.;-]{0,63}(>=|<=|~=|=)", value[position:])
            if match is None:
                raise cv.Invalid("LDAP filter needs a valid attribute and comparison")
            comparison = match.group(1)
            position += match.end()
            segment = 0
            wildcards = 0
            while position < len(value) and value[position] != ")":
                char = value[position]
                if char == "\\":
                    if not re.fullmatch(r"[0-9A-Fa-f]{2}", value[position + 1:position + 3]):
                        raise cv.Invalid("LDAP filter escapes use two hexadecimal digits")
                    position += 3
                    segment += 1
                    continue
                if char == "(" or char == "\0":
                    raise cv.Invalid("Escape LDAP filter parentheses and NUL bytes")
                if char == "*":
                    if comparison != "=":
                        raise cv.Invalid("LDAP substring wildcards apply only to equality")
                    segment = 0
                    wildcards += 1
                    if wildcards > 32:
                        raise cv.Invalid("LDAP substring filter has too many segments")
                else:
                    segment += 1
                    if segment > 256:
                        raise cv.Invalid("LDAP filter value segment exceeds 256 bytes")
                position += 1
        if position >= len(value) or value[position] != ")":
            raise cv.Invalid("Unbalanced LDAP filter")
        return position + 1

    if one(0, 0) != len(value):
        raise cv.Invalid("Unexpected data after LDAP filter")
    return value


def validate_config(config):
    has_external = bool(config.get("username") or config.get("password"))
    if has_external and not (config.get("username") and config.get("password")):
        raise cv.Invalid("Configure both username and password for explicit external LDAP credentials")
    if has_external and config.get("account_id"):
        raise cv.Invalid("Choose account_id or explicit LDAP username/password; shared account credentials are read-only")
    if not has_external and "account_id" not in config:
        config["account_id"] = "ha"
    if config["tls"]:
        config["tls_support"] = True
    if config["managed_by_ha"]:
        if config.get("account_id") != "ha":
            raise cv.Invalid("managed_by_ha requires account_id: ha")
        config["api_actions"] = True
    if config["api_actions"]:
        api = (CORE.raw_config or {}).get("api", {})
        if api is None or not api.get("custom_services", False):
            raise cv.Invalid("Native LDAP actions require api: custom_services: true; disable api_actions for standalone use")
    return config


CONFIG_SCHEMA = cv.All(cv.require_esphome_version(2026, 10, 0), cv.Schema({
    cv.GenerateID(): cv.declare_id(LdapDirectory),
    cv.GenerateID("voip_stack_id"): cv.use_id(VoipStack),
    cv.Optional("account_id"): lambda v: bounded(v, 32),
    cv.Optional("host", default=""): server,
    cv.Optional("port", default=389): cv.port,
    cv.Optional("tls", default=False): cv.boolean,
    cv.Optional("tls_support", default=False): cv.boolean,
    cv.Optional("ca_certificate", default=""): cv.string_strict,
    cv.Optional("username", default=""): lambda v: bounded(v, 1024),
    cv.Optional("password", default=""): lambda v: bounded(v, 256),
    cv.Optional("base_dn", default=""): bounded,
    cv.Optional("bind_dn", default=""): bounded,
    cv.Optional("filter", default="(|(telephoneNumber=*)(labeledURI=*))"): validate_filter,
    cv.Optional("max_entries", default=64): cv.int_range(min=1, max=64),
    cv.Optional("receive_buffer_size", default=8192): cv.int_range(min=1024, max=65536),
    cv.Optional("timeout", default="5s"): cv.positive_time_period_milliseconds,
    cv.Optional("refresh_interval", default="5min"): cv.positive_time_period_milliseconds,
    cv.Optional("task_core", default=-1): cv.int_range(min=-1, max=1),
    cv.Optional("task_priority", default=2): cv.int_range(min=1, max=20),
    cv.Optional("task_stack_size", default=8192): cv.int_range(min=4096, max=32768),
    cv.Optional("task_stack_in_psram", default=False): cv.boolean,
    cv.Optional("buffer_in_psram", default=True): cv.boolean,
    cv.Optional("api_actions", default=False): cv.boolean,
    cv.Optional("managed_by_ha", default=False): cv.boolean,
    cv.Optional("on_update"): automation.validate_automation(),
    cv.Optional("on_error"): automation.validate_automation(),
}).extend(cv.COMPONENT_SCHEMA), validate_config)


def final_validate(config):
    from esphome.components import socket
    socket.consume_sockets(1, "ldap_directory")(config)
    if not config.get("account_id"):
        return config
    full = fv.full_config.get()
    path = full.get_path_for_id(config["voip_stack_id"])
    parent = full.get_config_for_path(path[:-1])
    ha = parent.get("ha_integration") or {}
    if config["managed_by_ha"] and not (isinstance(ha, dict) and ha.get("auto_register")):
        raise cv.Invalid("managed_by_ha requires voip_stack.ha_integration.auto_register: true")
    if "registration" not in parent and not (isinstance(ha, dict) and ha.get("auto_register")):
        raise cv.Invalid("LDAP account_id requires voip_stack registration or ha_integration.auto_register; use explicit external credentials otherwise")
    return config


FINAL_VALIDATE_SCHEMA = final_validate


async def to_code(config):
    cg.add_define("USE_ESPHOME_VOIP_DIRECTORY")
    if config["tls_support"]:
        cg.add_define("USE_PHONE_CLIENT_TLS")
        esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    if config["task_stack_in_psram"]:
        esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY", True)
    if config["api_actions"]:
        cg.add_define("USE_LDAP_DIRECTORY_API")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config["voip_stack_id"])
    cg.add(var.set_parent(parent))
    cg.add(var.set_account_id(config.get("account_id", "")))
    cg.add(var.set_managed_by_ha(config["managed_by_ha"]))
    cg.add(var.set_server(config["host"], config["port"], config["tls"]))
    cg.add(var.set_directory(config["base_dn"], config["bind_dn"], config["filter"]))
    cg.add(var.set_credentials(config["username"], config["password"]))
    cg.add(var.set_ca_certificate(config["ca_certificate"]))
    cg.add(var.set_timeout(config["timeout"].total_milliseconds))
    cg.add(var.set_refresh_interval(config["refresh_interval"].total_milliseconds))
    for key in ("max_entries", "receive_buffer_size", "task_core", "task_priority", "task_stack_size", "task_stack_in_psram", "buffer_in_psram"):
        cg.add(getattr(var, f"set_{key}")(config[key]))
    for event in ("update", "error"):
        for conf in config.get(f"on_{event}", []):
            await automation.build_automation(getattr(var, f"get_{event}_trigger")(), [(cg.uint32, "value")], conf)


@automation.register_action("ldap_directory.refresh", RefreshAction, cv.Schema({cv.GenerateID(): cv.use_id(LdapDirectory)}), synchronous=True)
async def refresh_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
