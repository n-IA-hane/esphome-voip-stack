"""Optional PBX registration; omitted configurations emit no registration code."""
import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.core import CORE, ID
from esphome.components import esp32

CONF_REGISTRATION = "registration"
NS = cg.esphome_ns.namespace("voip_stack")


def account_id(value):
    value = cv.string_strict(value)
    if value == "direct" or not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", value):
        raise cv.Invalid("account_id must contain 1-32 letters, digits, dots, underscores or hyphens; direct is reserved")
    return value


def host(value):
    value = cv.string_strict(value)
    if len(value) > 253 or not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?", value):
        raise cv.Invalid("Use an IPv4 address or DNS hostname; configure the port separately")
    return value


def bounded(value, limit=128):
    value = cv.string_strict(value)
    if len(value.encode()) > limit or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise cv.Invalid(f"Value must contain at most {limit} UTF-8 bytes and no control characters")
    return value


def user(value):
    value = bounded(value, 64)
    if not value or re.search(r'[\s<>"\\;,@/?#:]', value):
        raise cv.Invalid("Use a SIP username without URI delimiters")
    return value


def complete_account(value):
    value = dict(value)
    value.setdefault("domain", value["registrar"])
    value.setdefault("auth_username", value["username"])
    return value


ACCOUNT_SCHEMA = cv.All(cv.Schema({
    cv.Required("account_id"): account_id,
    cv.Required("registrar"): host,
    cv.Optional("port", default=5060): cv.port,
    cv.Optional("domain"): host,
    cv.Required("username"): user,
    cv.Required("password"): bounded,
    cv.Optional("auth_username"): lambda v: bounded(v, 64),
    cv.Optional("proxy", default=""): cv.Any("", host),
    cv.Optional("proxy_port", default=5060): cv.port,
    cv.Optional("transport", default="udp"): cv.one_of("udp", "tcp", lower=True),
    cv.Optional("expires", default=3600): cv.int_range(min=30, max=86400),
    cv.Optional("enabled", default=True): cv.boolean,
}), complete_account)


def validate_registration(value):
    ids = [a["account_id"] for a in value["accounts"]]
    if len(ids) != len(set(ids)):
        raise cv.Invalid("PBX account_id values must be unique")
    if len(ids) > value["max_accounts"]:
        raise cv.Invalid("Increase registration.max_accounts to fit the configured accounts")
    if "default_line" not in value:
        value["default_line"] = ids[0] if not value["accept_direct_calls"] and len(ids) == 1 else "direct"
    if value["default_line"] != "direct" and value["default_line"] not in ids:
        raise cv.Invalid("default_line must be direct or the account_id of a configured account")
    return value


SCHEMA = cv.All(cv.Schema({
    cv.Optional("message_waiting", default=False): cv.boolean,
    cv.Optional("max_accounts", default=2): cv.int_range(min=1, max=4),
    cv.Optional("accept_direct_calls", default=True): cv.boolean,
    cv.Optional("default_line"): cv.Any("direct", account_id),
    cv.Optional("accounts", default=[]): cv.ensure_list(ACCOUNT_SCHEMA),
}), validate_registration)


async def to_code(parent, config):
    if CONF_REGISTRATION not in config:
        if "on_registration_state" in config or "on_message_waiting" in config:
            raise cv.Invalid("on_registration_state requires voip_stack.registration")
        return
    if not CORE.using_toolchain_esp_idf:
        raise cv.Invalid("SIP registration requires the ESP-IDF framework")
    conf = config[CONF_REGISTRATION]
    cg.add_define("USE_ESPHOME_VOIP_REGISTRATION")
    if conf["message_waiting"]:
        cg.add_define("USE_ESPHOME_VOIP_MWI")
    elif "on_message_waiting" in config:
        raise cv.Invalid("on_message_waiting requires registration.message_waiting: true")
    cg.add_define("VOIP_REGISTRATION_CAPACITY", conf["max_accounts"])
    esp32.include_builtin_idf_component("mbedtls")
    cg.add(parent.configure_registration(conf["accept_direct_calls"], conf["default_line"]))
    if conf["accounts"]:
        view_type = NS.struct("SipAccountConfigView")
        rows = [cg.StructInitializer(view_type,
            ("id", a["account_id"]), ("registrar", a["registrar"]),
            ("domain", a["domain"]), ("username", a["username"]),
            ("password", a["password"]), ("auth_username", a["auth_username"]),
            ("proxy", a["proxy"]), ("port", a["port"]),
            ("proxy_port", a["proxy_port"]), ("expires", a["expires"]),
            ("tcp", a["transport"] == "tcp"), ("enabled", a["enabled"]))
            for a in conf["accounts"]]
        accounts = cg.static_const_array(
            ID(f"{config['id']}_registration_accounts", is_declaration=True,
               type=view_type), rows)
        cg.add(parent.set_registration_accounts(accounts, len(rows)))
    for conf in config.get("on_registration_state", []):
        await automation.build_automation(parent.get_registration_state_trigger(),
            [(cg.std_string, "account_id"), (cg.std_string, "state"), (cg.int_, "status_code")], conf)
    for conf in config.get("on_message_waiting", []):
        await automation.build_automation(parent.get_message_waiting_trigger(),
            [(cg.std_string, "account_id"), (cg.bool_, "available"), (cg.bool_, "waiting"),
             (cg.uint32, "new_messages"), (cg.uint32, "old_messages"),
             (cg.uint32, "urgent_new"), (cg.uint32, "urgent_old")], conf)


def register_automations(parent_type):
    def require_registration():
        if CONF_REGISTRATION not in ((CORE.config or {}).get("voip_stack") or {}):
            raise cv.Invalid("This action requires voip_stack.registration")

    async def parented(conf, action_id, template_arg):
        require_registration()
        action = cg.new_Pvariable(action_id, template_arg)
        parent = await cg.get_variable(conf["id"])
        cg.add(action.set_parent(parent))
        return action

    operation = NS.class_("RegistrationAccountAction", automation.Action)
    names = {"register_account": 0, "unregister_account": 1, "remove_account": 2, "reset_account": 3, "select_line": 4}
    for name, code in names.items():
        fields = {cv.GenerateID(): cv.use_id(parent_type), cv.Required("line" if name == "select_line" else "account_id"): cv.templatable(cv.Any("direct", account_id))}
        if name == "remove_account":
            fields[cv.Optional("persist", default=False)] = cv.templatable(cv.boolean)

        def register(name, code, fields):
            @automation.register_action("voip_stack." + name, operation, cv.Schema(fields), synchronous=True)
            async def action_to_code(conf, action_id, template_arg, args):
                action = await parented(conf, action_id, template_arg)
                cg.add(action.set_operation(code))
                cg.add(action.set_account_id(await cg.templatable(conf["line" if name == "select_line" else "account_id"], args, cg.std_string)))
                if "persist" in conf:
                    cg.add(action.set_persist(await cg.templatable(conf["persist"], args, cg.bool_)))
                return action
        register(name, code, fields)

    start = NS.class_("StartCallOnLineAction", automation.Action)
    @automation.register_action("voip_stack.start_call_on_line", start, cv.Schema({
        cv.GenerateID(): cv.use_id(parent_type),
        cv.Required("target"): cv.templatable(cv.string),
        cv.Required("line"): cv.templatable(cv.Any("direct", account_id)),
    }), synchronous=True)
    async def call_to_code(conf, action_id, template_arg, args):
        action = await parented(conf, action_id, template_arg)
        cg.add(action.set_target(await cg.templatable(conf["target"], args, cg.std_string)))
        cg.add(action.set_line(await cg.templatable(conf["line"], args, cg.std_string)))
        return action

    condition = NS.class_("IsRegisteredCondition", automation.Condition)
    @automation.register_condition("voip_stack.is_registered", condition, cv.Schema({
        cv.GenerateID(): cv.use_id(parent_type), cv.Required("account_id"): cv.templatable(account_id),
    }))
    async def registered_to_code(conf, condition_id, template_arg, args):
        action = await parented(conf, condition_id, template_arg)
        cg.add(action.set_account_id(await cg.templatable(conf["account_id"], args, cg.std_string)))
        return action

    setter = NS.class_("SetAccountAction", automation.Action)
    @automation.register_action("voip_stack.set_account", setter, cv.Schema({
        cv.GenerateID(): cv.use_id(parent_type),
        cv.Required("account"): ACCOUNT_SCHEMA,
        cv.Optional("persist", default=False): cv.templatable(cv.boolean),
    }), synchronous=True)
    async def set_to_code(conf, action_id, template_arg, args):
        action = await parented(conf, action_id, template_arg)
        a = conf["account"]
        cg.add(action.set_config(a["account_id"], a["registrar"], a["port"], a["domain"], a["username"], a["password"],
            a["auth_username"], a["proxy"], a["proxy_port"], a["transport"] == "tcp", a["expires"], a["enabled"]))
        cg.add(action.set_persist(await cg.templatable(conf["persist"], args, cg.bool_)))
        return action


def register_mwi_condition(parent_type):
    condition = NS.class_("HasWaitingMessagesCondition", automation.Condition)
    @automation.register_condition("voip_stack.has_waiting_messages", condition, cv.Schema({
        cv.GenerateID(): cv.use_id(parent_type),
        cv.Optional("account_id", default=""): cv.templatable(cv.Any("", account_id)),
    }))
    async def condition_to_code(conf, condition_id, template_arg, args):
        if not (((CORE.config or {}).get("voip_stack") or {}).get(CONF_REGISTRATION) or {}).get("message_waiting", False):
            raise cv.Invalid("voip_stack.has_waiting_messages requires registration.message_waiting: true")
        action = cg.new_Pvariable(condition_id, template_arg)
        cg.add(action.set_parent(await cg.get_variable(conf["id"])))
        cg.add(action.set_account_id(await cg.templatable(conf["account_id"], args, cg.std_string)))
        return action
