#!/usr/bin/env python3
"""Use the existing ESPHome API for repeatable silent PBX account/call checks.

PBX lifecycle and SIP/RTP capture stay with the calling qualification harness.
This tool creates no server and reads API credentials from the private firmware
configuration. Account credentials are read from a private JSON file.
"""
from __future__ import annotations

import argparse
from email.parser import Parser
import asyncio
import inspect
import json
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.request import urlopen


async def maybe_await(value):
    return await value if inspect.isawaitable(value) else value


class BareSipWire:
    """Observe an existing peer's SIP trace through its PTY readiness events."""
    def __init__(self, peer):
        self.peer = peer
        self.loop = asyncio.get_running_loop()
        self.changed = asyncio.Event()
        self.loop.add_reader(peer.master, self.drain)

    def drain(self):
        try:
            self.peer.read()
        except (OSError, ValueError):
            self.loop.remove_reader(self.peer.master)
        self.changed.set()

    def records(self, since=0):
        text = re.sub(r"\x1b\[[0-9;]*m", "", self.peer.output[since:]).replace("\r\r\n", "\r\n")
        pattern = r"(?m)^(SIP/2\.0 [^\r\n]+|(?:INVITE|ACK|REFER|NOTIFY|BYE|REGISTER|OPTIONS|UPDATE|CANCEL) [^\r\n]+ SIP/2\.0)\r?\n"
        result = []
        for match in re.finditer(pattern, text):
            separator = re.search(r"\r?\n\r?\n", text[match.end():])
            if separator is None:
                continue
            end = match.end() + separator.start()
            body_start = match.end() + separator.end()
            headers = Parser().parsestr(text[match.end():end])
            size = int(headers.get("Content-Length", "0"))
            payload = text[body_start:].encode("utf-8")
            if len(payload) < size:
                continue
            body = payload[:size].decode("utf-8", errors="replace")
            result.append({"line": match.group(1), "headers": headers, "body": body})
        return result

    async def wait_record(self, predicate, since=0, timeout=15):
        deadline = time.monotonic() + timeout
        while True:
            for record in self.records(since):
                if predicate(record):
                    return record
            self.changed.clear()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("bareSIP wire record deadline expired")
            await asyncio.wait_for(self.changed.wait(), remaining)

    async def wait_text(self, value, since=0, timeout=15):
        deadline = time.monotonic() + timeout
        while value not in self.peer.output[since:]:
            self.changed.clear()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("bareSIP trace text deadline expired")
            await asyncio.wait_for(self.changed.wait(), remaining)

    async def direction(self, command):
        first = len(self.peer.output)
        self.peer.command(command)
        request = await self.wait_record(lambda row: row["line"].startswith("INVITE "), first)
        identity, sequence = request["headers"]["Call-ID"], request["headers"]["CSeq"].split()[0]
        response = await self.wait_record(lambda row: row["line"].startswith("SIP/2.0 ") and
            row["headers"].get("Call-ID") == identity and row["headers"].get("CSeq") == sequence + " INVITE" and
            int(row["line"].split()[1]) >= 200, first)
        status = int(response["line"].split()[1])
        if not 200 <= status < 300:
            raise AssertionError(f"Remote direction update was rejected with SIP {status}")
        await self.wait_record(lambda row: row["line"].startswith("ACK ") and
            row["headers"].get("Call-ID") == identity and row["headers"].get("CSeq") == sequence + " ACK", first)
        return request

    def close(self):
        self.loop.remove_reader(self.peer.master)
        self.drain()


class EspWitness:
    def __init__(self, client):
        self.client = client
        self.entities = []
        self.services = {}
        self.values = {}
        self.log = []
        self.account_states = {}
        self.renderer_stats = {"decoded": 0, "errors": 0, "presented": 0}
        self.changed = asyncio.Event()
        self.state_key = None
        self.unsubscribe = None

    async def open(self):
        await self.client.connect(login=True)
        self.entities, services = await self.client.list_entities_services()
        self.services = {service.name: service for service in services}
        for entity in self.entities:
            if type(entity).__name__ == "TextSensorInfo" and (entity.object_id == "voip_state" or entity.name == "VoIP State"):
                self.state_key = entity.key
        await maybe_await(self.client.subscribe_states(self.on_state))
        from aioesphomeapi import LogLevel
        self.unsubscribe = self.client.subscribe_logs(self.on_log, log_level=LogLevel.LOG_LEVEL_INFO, dump_config=False)

    def on_state(self, state):
        # Keys are unique only inside an entity domain. A media player sharing
        # the text sensor's numeric key must not overwrite the call state.
        domain = type(state).__name__
        self.values[(domain, state.key)] = getattr(state, "state", None)
        self.changed.set()

    def on_log(self, entry):
        value = entry.message.decode(errors="replace") if isinstance(entry.message, bytes) else str(entry.message)
        value = re.sub(r"\x1b\[[0-9;]*m", "", value)
        if any(tag in value for tag in ("registration.hil", "voip_stack", "esp_screenshot", "esp_video_renderer", "runtime_controller")):
            self.log.append({"time": time.monotonic(), "message": value})
            self.changed.set()
        renderer = re.search(r"decoded/errors/LVGL: (\d+)/(\d+)/(\d+)", value)
        if renderer:
            self.renderer_stats = dict(zip(("decoded", "errors", "presented"), map(int, renderer.groups())))
        match = re.search(r"account=(\S+) state=(\S+) status=(\d+)", value)
        if match:
            self.account_states[match[1]] = match[2]
            self.changed.set()

    @property
    def call_state(self):
        return self.values.get(("TextSensorState", self.state_key), "")

    async def wait(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while not predicate():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("ESP witness deadline expired")
            self.changed.clear()
            if predicate():
                break
            await asyncio.wait_for(self.changed.wait(), remaining)

    async def service(self, name, data=None):
        if name not in self.services:
            raise RuntimeError(f"Running firmware does not expose {name}")
        await maybe_await(self.client.execute_service(self.services[name], data or {}))

    async def silence(self):
        for entity in self.entities:
            if type(entity).__name__ == "NumberInfo" and (entity.object_id == "master_volume" or entity.name == "Master Volume"):
                await maybe_await(self.client.number_command(entity.key, 0.0))
                return
        raise RuntimeError("Master Volume entity is absent; this runner requires silent state/media checks")

    def sensor(self, object_id, name):
        entity = next((e for e in self.entities if type(e).__name__ == "TextSensorInfo" and
                       (e.object_id == object_id or e.name == name)), None)
        return self.values.get(("TextSensorState", entity.key), "") if entity else ""

    async def screenshot(self, address, output):
        previous = self.sensor("screenshot_id", "Screenshot ID")
        await self.service("take_screenshot")
        await self.wait(lambda: self.sensor("screenshot_id", "Screenshot ID") not in {"", previous}, timeout=15)
        capture_id = self.sensor("screenshot_id", "Screenshot ID")
        def fetch():
            with urlopen(f"http://{address}/esp_screenshot/screen/image.jpg?capture={capture_id}", timeout=10) as response:
                return response.read()
        payload = await asyncio.to_thread(fetch)
        output.write_bytes(payload)
        return {"capture_id": capture_id, "bytes": len(payload), "file": output.name}

    async def wait_mww(self, expected, timeout=8):
        deadline = time.monotonic() + timeout
        token = "mww=YES" if expected else "mww=NO"
        while time.monotonic() < deadline:
            first = len(self.log)
            await self.service("registration_runtime_diagnostics")
            await self.wait(lambda: any("mww=" in item["message"] for item in self.log[first:]), timeout=3)
            if any(token in item["message"] for item in self.log[first:]):
                return
            await asyncio.sleep(0.5)
        raise TimeoutError(f"Native Micro Wake Word did not reach {token}")

    async def switch_value(self, entity):
        await self.wait(lambda: ("SwitchState", entity.key) in self.values, timeout=5)
        return bool(self.values[("SwitchState", entity.key)])

    async def close(self):
        if self.unsubscribe:
            self.unsubscribe()
            self.unsubscribe = None
        from aioesphomeapi.core import APIConnectionError
        try:
            await self.client.disconnect()
        except APIConnectionError:
            # A commanded hardware restart can close the stream before the
            # native DisconnectResponse reaches the cleanup code.
            pass


class AsteriskLab:
    """A private native PBX used only for the explicit ESP matrix operation."""
    def __init__(self, address, port, documentation):
        self.address, self.port, self.documentation = address, port, documentation
        self.temporary = tempfile.TemporaryDirectory(prefix="esp-registration-pbx-")
        self.root = Path(self.temporary.name)
        self.password = secrets.token_hex(24)
        self.process = None
        self.log = None

    def command(self, text):
        output = subprocess.run(["asterisk", "-C", str(self.root / "asterisk.conf"), "-rx", text],
                                capture_output=True, text=True, check=True).stdout
        if "No such command" in output:
            raise RuntimeError(f"Private PBX command is unavailable: {text.split()[0:2]}")
        return output

    async def start(self):
        if shutil.which("asterisk") is None:
            raise RuntimeError("Native Asterisk is required")
        for name in ("etc", "run", "var", "log", "spool", "data", "keys", "agi"):
            (self.root / name).mkdir(exist_ok=True)
        if not (self.root / "data/documentation").exists():
            (self.root / "data/documentation").symlink_to(self.documentation, target_is_directory=True)
        directories = {"astetcdir": "etc", "astvarlibdir": "var", "astdbdir": "var", "astkeydir": "keys",
                       "astdatadir": "data", "astagidir": "agi", "astspooldir": "spool",
                       "astrundir": "run", "astlogdir": "log"}
        (self.root / "asterisk.conf").write_text("[directories]\nastmoddir => /usr/lib/asterisk/modules\n" +
            "".join(f"{key} => {self.root / directory}\n" for key, directory in directories.items()) +
            "[options]\nverbose=0\ndebug=0\n")
        modules = ["res_pjproject", "res_sorcery_config", "res_sorcery_memory", "res_sorcery_astdb", "res_pjsip",
                   "res_pjsip_authenticator_digest", "res_pjsip_outbound_authenticator_digest",
                   "res_pjsip_endpoint_identifier_user", "res_pjsip_registrar", "res_pjsip_pubsub", "res_rtp_asterisk",
                   "res_pjsip_session", "res_pjsip_sdp_rtp", "chan_pjsip", "pbx_config", "app_echo",
                   "app_dial", "codec_resample", "res_clioriginate", "res_pjsip_logger"]
        (self.root / "etc/modules.conf").write_text("[modules]\nautoload=no\n" +
            "".join(f"load={name}.so\n" for name in modules))
        (self.root / "etc/rtp.conf").write_text("[general]\nrtpstart=18000\nrtpend=18100\n")
        (self.root / "etc/extensions.conf").write_text("[test]\nexten => 9000,1,Answer()\n same => n,Echo()\n same => n,Hangup()\n")
        text = "".join(f"[transport-{kind}]\ntype=transport\nprotocol={kind}\nbind={self.address}:{self.port}\n"
                       for kind in ("udp", "tcp"))
        for number, kind, algorithm in (("7101", "udp", "MD5"), ("7102", "tcp", "SHA-256")):
            text += (f"[{number}]\ntype=endpoint\ntransport=transport-{kind}\ncontext=test\n"
                     f"auth={number}-auth\naors={number}\ndisallow=all\nallow=slin16\nuse_ptime=yes\ndirect_media=no\n"
                     f"[{number}-auth]\ntype=auth\nauth_type=digest\nusername={number}\npassword={self.password}\n"
                     f"supported_algorithms_uas={algorithm}\n"
                     f"[{number}]\ntype=aor\nmax_contacts=2\nremove_existing=yes\nminimum_expiration=1\n")
        (self.root / "etc/pjsip.conf").write_text(text)
        self.log = (self.root / "pbx.log").open("ab")
        self.process = subprocess.Popen(["asterisk", "-f", "-C", str(self.root / "asterisk.conf")],
                                        stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("Private PBX exited during initialization")
            try:
                transports = self.command("pjsip show transports")
                if "transport-udp" in transports and "transport-tcp" in transports:
                    channel = self.command("module show like chan_pjsip")
                    if "chan_pjsip.so" not in channel or "Running" not in channel:
                        raise RuntimeError("Private PBX channel driver is not running; reject before touching the ESP accounts")
                    if "No such command" in self.command("core show help channel originate"):
                        raise RuntimeError("Private PBX originate command is absent")
                    self.command("pjsip set logger on")
                    return
            except (subprocess.CalledProcessError, RuntimeError):
                pass
            await asyncio.sleep(0.1)
        raise TimeoutError("Private PBX did not start")

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            try:
                self.command("core stop now")
                self.process.wait(timeout=5)
            except (subprocess.SubprocessError, OSError, RuntimeError):
                if self.process.poll() is None:
                    self.process.terminate()
                    try:
                        self.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait()
        if self.log:
            self.log.close()
            self.log = None

    def close(self, output):
        self.stop()
        log = self.root / "pbx.log"
        if log.exists():
            output.with_suffix(".asterisk.log").write_text(log.read_text(errors="replace").replace(self.password, "[redacted]"))
        self.temporary.cleanup()


def account_payload(account_id, registrar, port, username, password, transport, persist=False):
    return {"account_id": account_id, "registrar": registrar, "port": port, "domain": registrar,
            "username": username, "password": password, "auth_username": "", "proxy": "", "proxy_port": 5060,
            "transport": transport, "expires": 60, "persist": persist}


async def asterisk_matrix(witness, args, result):
    lab = AsteriskLab(args.pbx_address, args.pbx_port, args.asterisk_documentation)
    scenarios = []
    result["scenarios"] = scenarios
    wake = next((e for e in witness.entities if type(e).__name__ == "SwitchInfo" and e.object_id == "wake_word"), None)
    old_wake = await witness.switch_value(wake) if wake else False
    try:
        if wake:
            await maybe_await(witness.client.switch_command(wake.key, True))
            await witness.wait_mww(True)
        await lab.start()
        for account_id, username, transport in (("lab_udp", "7101", "udp"), ("lab_tcp", "7102", "tcp")):
            payload = account_payload(account_id, args.registrar_host or args.pbx_address, args.pbx_port,
                                      username, lab.password, transport)
            payload["domain"] = args.pbx_address
            payload["expires"] = args.expires
            await witness.service("set_account", payload)
        await witness.wait(lambda: all(witness.account_states.get(i) == "registered" for i in ("lab_udp", "lab_tcp")), timeout=35)
        contacts = lab.command("pjsip show contacts")
        if not all(i in contacts for i in ("7101", "7102", "esphome-lab_udp", "esphome-lab_tcp")):
            raise AssertionError("PBX binding oracle did not confirm both account Contacts")
        for account_id, username in (("lab_udp", "7101"), ("lab_tcp", "7102")):
            await witness.service("start_call_on_line", {"dest": "9000", "line": account_id})
            await witness.wait(lambda: witness.call_state == "in_call", timeout=25)
            await asyncio.sleep(args.hold)
            await witness.service("registration_runtime_diagnostics")
            await witness.service("hangup_call")
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
            scenarios.append({"line": account_id, "direction": "outgoing", "passed": True})
            # PBX originates on the registered Contact. Accept a manual-answer
            # endpoint through its existing native action, without changing its preference.
            lab.command(f"channel originate PJSIP/{username} application Echo")
            await witness.wait(lambda: witness.call_state in {"ringing", "in_call"}, timeout=25)
            if witness.call_state == "ringing":
                await witness.service("answer_call")
                await witness.wait(lambda: witness.call_state == "in_call", timeout=15)
            await asyncio.sleep(args.hold)
            await witness.service("registration_runtime_diagnostics")
            lab.command("channel request hangup all")
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
            scenarios.append({"line": account_id, "direction": "incoming", "passed": True})
        if args.restart_pbx:
            lab.stop()
            await witness.wait(lambda: witness.account_states.get("lab_tcp") == "retry", timeout=10)
            before = len(witness.log)
            await witness.service("register_account", {"account_id": "lab_tcp"})
            await witness.wait(lambda: any("SIP TCP connect to" in entry["message"] and "failed" in entry["message"]
                                          for entry in witness.log[before:]), timeout=12)
            await lab.start()
            for account_id in ("lab_udp", "lab_tcp"):
                await witness.service("register_account", {"account_id": account_id})
            # A refresh preserves REGISTERED and need not emit another changed
            # callback. The independent PBX binding table is the recovery oracle.
            deadline = time.monotonic() + 40
            while True:
                contacts = lab.command("pjsip show contacts")
                if all(i in contacts for i in ("esphome-lab_udp", "esphome-lab_tcp")):
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError("PBX restart did not restore both bindings")
                await asyncio.sleep(0.2)
            result["connection_refused_then_pbx_restart_recovered"] = True
        for account_id in ("lab_udp", "lab_tcp"):
            await witness.service("unregister_account", {"account_id": account_id})
            await witness.wait(lambda: witness.account_states.get(account_id) == "disabled", timeout=35)
            await witness.service("reset_account", {"account_id": account_id})
        if "sip:" in lab.command("pjsip show contacts"):
            raise AssertionError("PBX bindings survived unregister")
        result["scenarios"] = scenarios
        result["bindings_released"] = True
        result["mww_enabled_during_audio_matrix"] = wake is not None
    finally:
        try:
            if witness.call_state not in {"", "idle"}:
                await witness.service("hangup_call")
                await witness.wait(lambda: witness.call_state == "idle", timeout=15)
            for account_id in ("lab_udp", "lab_tcp"):
                await witness.service("unregister_account", {"account_id": account_id})
        except Exception as error:
            result["cleanup_error"] = type(error).__name__
        finally:
            lab.close(args.output)
            if wake:
                await maybe_await(witness.client.switch_command(wake.key, old_wake))


async def video_matrix(witness, args, result):
    if args.video_peer is None or not args.video_peer.is_file():
        raise ValueError("--video-peer must point to the existing maintained sip_video_peer.py")
    wake = next((e for e in witness.entities if type(e).__name__ == "SwitchInfo" and e.object_id == "wake_word"), None)
    if wake is None:
        raise RuntimeError("Wake Word switch is absent")
    old_wake = await witness.switch_value(wake)
    frames = []
    peer = None
    try:
        await maybe_await(witness.client.switch_command(wake.key, True))
        await witness.wait(lambda: witness.values.get(("SwitchState", wake.key)) is True, timeout=5)
        await witness.wait_mww(True)
        frames.append(await witness.screenshot(args.address, args.output.with_name("video-before.jpg")))
        for cycle in range(2):
            report = args.output.with_name(f"video-peer-{cycle + 1}.json")
            command = [sys.executable, str(args.video_peer), "--host", args.address,
                       "--port", "5060", "--target", "101", "--local-ip", args.pbx_address,
                       "--audio-codec", "l16-16k-10", "--codec", "jpeg", "--direction", "sendrecv",
                       "--video-fps", "2", "--duration", "9", "--out", str(report)]
            if cycle:
                command += ["--add-video-after", "2"]
            peer = await asyncio.create_subprocess_exec(*command, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            await witness.wait(lambda: witness.call_state in {"ringing", "in_call"}, timeout=20)
            if witness.call_state == "ringing":
                await witness.service("answer_call")
                await witness.wait(lambda: witness.call_state == "in_call", timeout=15)
            await asyncio.sleep(4)
            await witness.wait_mww(False)
            frames.append(await witness.screenshot(args.address, args.output.with_name(f"video-active-{cycle + 1}.jpg")))
            stdout, stderr = await asyncio.wait_for(peer.communicate(), 25)
            if peer.returncode:
                raise RuntimeError(f"Video peer failed (see report), exit {peer.returncode}")
            args.output.with_name(f"video-peer-{cycle + 1}.stdout").write_bytes(stdout)
            args.output.with_name(f"video-peer-{cycle + 1}.stderr").write_bytes(stderr)
            peer = None
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
            await witness.wait_mww(True)
            frames.append(await witness.screenshot(args.address, args.output.with_name(f"video-after-{cycle + 1}.jpg")))
        result["screenshots"] = frames
        result["video_cycles"] = 2
        result["mww_verified_running_before_and_after"] = True
    finally:
        if peer is not None and peer.returncode is None:
            peer.terminate()
            try:
                await asyncio.wait_for(peer.communicate(), 5)
            except asyncio.TimeoutError:
                peer.kill()
                await peer.communicate()
        if witness.call_state not in {"", "idle"}:
            await witness.service("hangup_call")
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
        await maybe_await(witness.client.switch_command(wake.key, old_wake))


async def browser_video_matrix(witness, args, result):
    """Observe an externally controlled ordinary HA browser call on the ESP."""
    wake = next((e for e in witness.entities if type(e).__name__ == "SwitchInfo" and e.object_id == "wake_word"), None)
    if wake is None:
        raise RuntimeError("Wake Word switch is absent")
    old_wake = await witness.switch_value(wake)
    frames = []
    try:
        await maybe_await(witness.client.switch_command(wake.key, True))
        await witness.wait_mww(True)
        baseline = dict(witness.renderer_stats)
        frames.append(await witness.screenshot(args.address, args.output.with_name("browser-video-before.jpg")))
        print("ESP_VIDEO_OBSERVER_READY", flush=True)
        await witness.wait(lambda: witness.call_state in {"ringing", "in_call"}, timeout=180)
        if witness.call_state == "ringing":
            await witness.service("answer_call")
        await witness.wait(lambda: witness.call_state == "in_call", timeout=20)
        await witness.wait_mww(False)
        await asyncio.sleep(3)
        frames.append(await witness.screenshot(args.address, args.output.with_name("browser-video-active.jpg")))
        await witness.service("registration_runtime_diagnostics")
        await witness.wait(lambda: witness.call_state == "idle", timeout=40)
        await witness.wait_mww(True)
        frames.append(await witness.screenshot(args.address, args.output.with_name("browser-video-after.jpg")))
        delta = {key: witness.renderer_stats[key] - baseline[key] for key in baseline}
        result.update(screenshots=frames, renderer_frames=delta, mww_before_and_after=True, mww_suspended_during_video=True)
        if delta["presented"] <= 0 or delta["errors"] != 0:
            raise AssertionError(f"Video transport completed without valid presentation: {delta}")
    finally:
        if witness.call_state not in {"", "idle"}:
            await witness.service("hangup_call")
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
        await maybe_await(witness.client.switch_command(wake.key, old_wake))


async def ha_matrix(witness, args, result):
    """Real HA registrar plus an ordinary bareSIP peer, using existing owners."""
    if args.ha_peer is None or args.account is None:
        raise ValueError("--ha-peer and --account are required")
    sys.path.insert(0, str(args.intercom / "tools"))
    from ha_softphone_matrix import BareSip
    account = json.loads(args.account.read_text())
    account_id = account["account_id"]
    payload = account_payload(account_id, account["registrar"], account.get("port", 5060),
                              account["username"], account["password"], account.get("transport", "udp"))
    payload["domain"] = account.get("domain", account["registrar"])
    scenarios = result.setdefault("scenarios", [])
    phases = set(args.ha_only or ("calls", "hold", "refer", "voicemail"))
    peer = None
    wire = None
    wake = next((e for e in witness.entities if type(e).__name__ == "SwitchInfo" and e.object_id == "wake_word"), None)
    old_wake = await witness.switch_value(wake) if wake else False
    async def established(peer_since=None):
        await witness.wait(lambda: witness.call_state in {"ringing", "in_call"}, timeout=20)
        if witness.call_state == "ringing":
            await witness.service("answer_call")
        await witness.wait(lambda: witness.call_state == "in_call", timeout=20)
        if peer_since is not None:
            await wire.wait_text("Call established:", peer_since)
    async def complete(name, *, peer_hangup=True):
        await asyncio.sleep(args.hold)
        await witness.service("registration_runtime_diagnostics")
        if peer_hangup:
            await asyncio.to_thread(peer.hangup)
        else:
            await witness.service("hangup_call")
        await witness.wait(lambda: witness.call_state == "idle", timeout=15)
        scenarios.append({"name": name, "passed": True})
    try:
        if wake:
            await maybe_await(witness.client.switch_command(wake.key, True))
            await witness.wait_mww(True)
        if not args.reuse_account:
            await witness.service("set_account", payload)
            await witness.wait(lambda: witness.account_states.get(account_id) == "registered", timeout=35)
        peer = await asyncio.to_thread(BareSip, args.ha_peer)
        wire = BareSipWire(peer)
        peer.command("/siptrace yes")
        await wire.wait_text("SIP trace is now enabled")
        if "calls" in phases:
            peer_since = len(peer.output)
            await asyncio.to_thread(peer.command, f"/dial sip:{args.ha_extension}@{account['registrar']}:{account.get('port', 5060)}")
            await established(peer_since)
            await complete("HA registered incoming, peer hangup")
            peer_since = len(peer.output)
            await witness.service("select_line", {"line": account_id})
            await witness.service("start_call", {"dest": args.peer_extension})
            await established(peer_since)
            await witness.service("select_line", {"line": "direct"})
            await complete("HA selected-line outgoing, active account retained after selecting direct, ESP hangup", peer_hangup=False)
        if "cancel" in phases:
            first = len(peer.output)
            peer.command("/answermode manual")
            await wire.wait_text("Answer mode changed to: manual", first)
            first = len(peer.output)
            await witness.service("start_call_on_line", {"dest": args.peer_extension, "line": account_id})
            request = await wire.wait_record(lambda row: row["line"].startswith("INVITE "), first)
            await wire.wait_text("Incoming call from:", first)
            await witness.service("hangup_call")
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
            identity = request["headers"]["Call-ID"]
            sequence = request["headers"]["CSeq"].split()[0]
            await wire.wait_record(lambda row: row["line"].startswith("SIP/2.0 487") and
                row["headers"].get("Call-ID") == identity, first)
            await wire.wait_record(lambda row: row["line"].startswith("ACK ") and
                row["headers"].get("Call-ID") == identity and row["headers"].get("CSeq") == sequence + " ACK", first)
            scenarios.append({"name": "Outgoing ringing call cancelled, final487 acknowledged", "passed": True})
            peer.command("/answermode auto")
        if "hold" in phases:
            peer_since = len(peer.output)
            await asyncio.to_thread(peer.command, f"/dial sip:esphome-{account_id}@{args.address}:5060")
            await established(peer_since)
            await wire.direction("/hold")
            # Exceed the ordinary 15-second RTP watchdog while held.
            await asyncio.sleep(17)
            await witness.service("registration_runtime_diagnostics")
            if witness.call_state != "in_call":
                raise AssertionError("Hold unexpectedly terminated the ESP call")
            await wire.direction("/resume")
            await complete("Direct registered Contact hold/resume, peer hangup")
        if "refer" in phases:
            peer_since = len(peer.output)
            await asyncio.to_thread(peer.command, f"/dial sip:esphome-{account_id}@{args.address}:5060")
            await established(peer_since)
            held = await wire.direction("/hold")
            original_id = held["headers"]["Call-ID"]
            first = len(peer.output)
            peer.command(f"/transfer sip:{args.peer_extension}@{account['registrar']}:{account.get('port', 5060)}")
            await wire.wait_text("Incoming call from:", first)
            peer.command("/accept")
            notify = await wire.wait_record(lambda row: row["line"].startswith("NOTIFY ") and
                row["headers"].get("Call-ID") == original_id and
                row["headers"].get("Event", "").startswith("refer") and
                re.match(r"SIP/2\.0 [2-6]\d\d", row["body"]) is not None, first, 25)
            status = int(notify["body"].split()[1])
            if not 200 <= status < 300:
                raise AssertionError(f"REFER replacement failed with SIP {status}")
            result["refer_final_notify_status"] = status
            await complete("ESP receives blind REFER, authenticated replacement call through HA", peer_hangup=False)
        if "voicemail" in phases:
            before = len(witness.log)
            await witness.service("start_call_on_line", {"dest": "*60" + args.ha_extension, "line": account_id})
            await established()
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
            await witness.wait(lambda: any(f"mwi account={account_id}" in row["message"] and "waiting=YES" in row["message"]
                                          for row in witness.log[before:]), timeout=8)
            scenarios.append({"name": "HA mailbox deposit and physical ESP message-summary notification", "passed": True})
            if args.retrieve:
                before = len(witness.log)
                await witness.service("start_call_on_line", {"dest": "*97", "line": account_id})
                await established()
                await witness.wait(lambda: any(f"mwi account={account_id}" in row["message"] and
                                               "waiting=NO new=0" in row["message"]
                                               for row in witness.log[before:]), timeout=35)
                await witness.service("registration_runtime_diagnostics")
                if witness.call_state != "idle":
                    await witness.service("hangup_call")
                    await witness.wait(lambda: witness.call_state == "idle", timeout=15)
                scenarios.append({"name": "HA voicemail first-message playback and read notification", "passed": True})
    finally:
        await witness.service("select_line", {"line": "direct"})
        if witness.call_state not in {"", "idle"}:
            await witness.service("hangup_call")
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
        if peer is not None:
            if wire is not None:
                wire.close()
            await asyncio.to_thread(peer.close)
            credentials = json.loads((args.ha_peer / "credentials.json").read_text())
            log = peer.output
            for key in ("password", "sip_password"):
                value = credentials.get(key)
                if value:
                    log = log.replace(value, "[redacted]")
            args.output.with_suffix(".baresip.log").write_text(log)
        if wake:
            await maybe_await(witness.client.switch_command(wake.key, old_wake))


async def persistence_matrix(witness, args, result):
    """Qualify the real ESP NVS overlay and YAML restoration across two boots."""
    if args.account is None:
        raise ValueError("--account is required")
    account = json.loads(args.account.read_text())
    account_id = account["account_id"]
    payload = account_payload(account_id, account["registrar"], account.get("port", 5060),
                              account["username"], account["password"], account.get("transport", "udp"), True)
    payload["domain"] = account.get("domain", account["registrar"])
    extension = next((e for e in witness.entities if type(e).__name__ == "TextInfo" and
                      (e.object_id == "extension" or e.name == "Extension")), None)
    original_extension = None
    if extension is not None:
        await witness.wait(lambda: ("TextState", extension.key) in witness.values, timeout=5)
        original_extension = witness.values[("TextState", extension.key)]
        before = len(witness.log)
        await maybe_await(witness.client.text_command(extension.key, "012345678901234567890123456789"))
        await witness.wait(lambda: any("Extension exceeds" in row["message"] for row in witness.log[before:]), timeout=5)
        if witness.values.get(("TextState", extension.key)) != original_extension:
            raise AssertionError("Rejected extension edit changed the published value")
        result["runtime_extension_overflow_rejected_atomically"] = True
    async def reboot():
        button = next((e for e in witness.entities if type(e).__name__ == "ButtonInfo" and
                       (e.object_id == "restart" or e.name == "Restart")), None)
        if button is None:
            raise RuntimeError("The existing Restart button is absent")
        await maybe_await(witness.client.button_command(button.key))
        await witness.close()
        witness.values.clear()
        witness.account_states.clear()
        deadline = time.monotonic() + 40
        while True:
            try:
                await witness.open()
                await witness.wait(lambda: witness.call_state == "idle", timeout=10)
                await witness.silence()
                return
            except Exception:
                await witness.close()
                if time.monotonic() >= deadline:
                    raise
                await asyncio.sleep(1)
    result["stage"] = "retire_previous_binding"
    await witness.service("unregister_account", {"account_id": account_id})
    await witness.wait(lambda: witness.account_states.get(account_id) == "disabled", timeout=35)
    result["stage"] = "write_persistent_account"
    await witness.service("set_account", payload)
    await witness.wait(lambda: witness.account_states.get(account_id) == "registered", timeout=35)
    before = len(witness.log)
    result["stage"] = "restart_and_wait_for_autoregistration"
    await reboot()
    await witness.service("registration_runtime_diagnostics")
    await witness.wait(lambda: witness.account_states.get(account_id) == "registered" or
                       any("ha=YES" in row["message"] for row in witness.log[before:]), timeout=75)
    await witness.service("registration_runtime_diagnostics")
    result["persisted_account_autoregistered_after_hardware_restart"] = True
    result["stage"] = "clear_override_and_restart"
    await witness.service("unregister_account", {"account_id": account_id})
    await witness.wait(lambda: witness.account_states.get(account_id) == "disabled", timeout=35)
    await witness.service("reset_account", {"account_id": account_id})
    before = len(witness.log)
    await reboot()
    await witness.service("registration_runtime_diagnostics")
    await witness.wait(lambda: any("ha=NO" in row["message"] for row in witness.log[before:]), timeout=15)
    result["reset_removed_persistent_override_and_restored_empty_yaml_after_restart"] = True
    result["stage"] = "complete"
    if extension is not None and witness.values.get(("TextState", extension.key)) != original_extension:
        raise AssertionError("Rejected extension edit changed the saved value across restart")


def api_parameters(config_path, address, expected_mac):
    from esphome.core import CORE
    from esphome.config import read_config
    CORE.config_path = Path(config_path)
    config = read_config({}, skip_external_update=True)
    if config is None or "api" not in config:
        raise ValueError("An existing ESPHome configuration with api is required")
    api = config["api"]
    return {
        "address": address,
        "port": api.get("port", 6053),
        "password": api.get("password", ""),
        "noise_psk": api.get("encryption", {}).get("key"),
        "expected_mac": expected_mac,
    }


async def qualify(args):
    from aioesphomeapi import APIClient
    witness = EspWitness(APIClient(**api_parameters(args.config, args.address, args.expected_mac)))
    account = json.loads(args.account.read_text()) if args.account else {}
    account_id = account.get("account_id", args.line)
    result = {"operation": args.operation, "passed": False, "physical_acoustic_qualification": False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        await witness.open()
        info = await witness.client.device_info()
        if info.mac_address.replace(":", "").lower() != args.expected_mac.replace(":", "").lower():
            raise RuntimeError("API MAC differs from the approved physical target")
        result["device"] = {"mac": info.mac_address, "name": info.name, "build": info.compilation_time,
                            "esphome": info.esphome_version}
        await witness.silence()
        await witness.wait(lambda: witness.call_state != "", timeout=10)
        result["initial_call_state"] = witness.call_state
        if args.operation != "observe" and witness.call_state != "idle":
            raise RuntimeError("The device is not idle; leave the existing call untouched")
        if args.operation == "provision":
            if not account:
                raise ValueError("--account is required")
            # The native ESPHome action has fixed arguments; defaults belong
            # here rather than silently omitting fields from the wire contract.
            fields = {"account_id": account_id, "registrar": account["registrar"], "port": account.get("port", 5060),
                "domain": account.get("domain", account["registrar"]), "username": account["username"],
                "password": account["password"], "auth_username": account.get("auth_username", ""),
                "proxy": account.get("proxy", ""), "proxy_port": account.get("proxy_port", 5060),
                "transport": account.get("transport", "udp"), "expires": account.get("expires", 60),
                "persist": args.persist}
            await witness.service("set_account", fields)
            await witness.wait(lambda: witness.account_states.get(account_id) in {"registered", "failed"}, timeout=35)
            if witness.account_states.get(account_id) != "registered":
                raise RuntimeError("The PBX account failed registration")
        elif args.operation == "call":
            if not args.target:
                raise ValueError("--target is required")
            await witness.service("start_call_on_line", {"dest": args.target, "line": account_id or "direct"})
            await witness.wait(lambda: witness.call_state == "in_call", timeout=25)
            await asyncio.sleep(args.hold)
            await witness.service("registration_runtime_diagnostics")
            await witness.service("hangup_call")
            await witness.wait(lambda: witness.call_state == "idle", timeout=15)
        elif args.operation == "remove":
            if not account_id:
                raise ValueError("--account or --line is required")
            await witness.service("unregister_account", {"account_id": account_id})
            await witness.wait(lambda: witness.account_states.get(account_id) == "disabled", timeout=35)
            await witness.service("reset_account", {"account_id": account_id})
        elif args.operation == "observe":
            await asyncio.sleep(args.hold)
        elif args.operation == "asterisk":
            await asterisk_matrix(witness, args, result)
        elif args.operation == "video":
            await video_matrix(witness, args, result)
        elif args.operation == "browser-video":
            await browser_video_matrix(witness, args, result)
        elif args.operation == "ha":
            await ha_matrix(witness, args, result)
        elif args.operation == "persist":
            await persistence_matrix(witness, args, result)
        await witness.service("registration_runtime_diagnostics")
        await asyncio.sleep(0.4)
        result.update(passed=True, final_call_state=witness.call_state, registration_states=witness.account_states)
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        if args.operation == "call" and witness.call_state not in {"", "idle"}:
            try:
                await witness.service("hangup_call")
                await witness.wait(lambda: witness.call_state == "idle", timeout=15)
            except Exception:
                result["call_cleanup_failed"] = True
    finally:
        result["logs"] = witness.log
        await witness.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # Never serialize account credentials or the execute_service payload.
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": result["passed"], "operation": args.operation,
                      "final_call_state": result.get("final_call_state", "unknown")}))
    return 0 if result["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--restart-pbx", action="store_true", help="Qualify recovery after a real refused TCP connection")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--address", required=True)
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--operation", choices=["observe", "provision", "call", "remove", "asterisk", "video", "browser-video", "ha", "persist"], required=True)
    parser.add_argument("--account", type=Path)
    parser.add_argument("--ha-peer", type=Path)
    parser.add_argument("--ha-only", action="append", choices=["calls", "cancel", "hold", "refer", "voicemail"])
    parser.add_argument("--reuse-account", action="store_true")
    parser.add_argument("--intercom", type=Path, default=Path(__file__).resolve().parents[2] / "esphome-intercom")
    parser.add_argument("--ha-extension", default="7192")
    parser.add_argument("--peer-extension", default="7193")
    parser.add_argument("--retrieve", action="store_true")
    parser.add_argument("--line", default="direct")
    parser.add_argument("--target")
    parser.add_argument("--hold", type=float, default=3)
    parser.add_argument("--persist", action="store_true")
    parser.add_argument("--pbx-address")
    parser.add_argument("--pbx-port", type=int, default=15170)
    parser.add_argument("--registrar-host", help="Optional DNS registrar hostname for the real ESP resolver check")
    parser.add_argument("--expires", type=int, default=60)
    parser.add_argument("--asterisk-documentation", type=Path, default=Path("/var/lib/asterisk/documentation"))
    parser.add_argument("--video-peer", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.operation in {"asterisk", "video"} and not args.pbx_address:
        parser.error("--pbx-address must be the local qualification peer address reachable from the ESP")
    return asyncio.run(qualify(args))


if __name__ == "__main__":
    raise SystemExit(main())
