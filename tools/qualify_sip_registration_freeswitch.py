#!/usr/bin/env python3
"""Qualify the production REGISTER engine against an isolated native FreeSWITCH.

Only registrar interoperability is measured, not ESP call or media behavior.
The PBX and its control socket bind loopback. Runtime credentials stay in a
private temporary directory and the probe environment, never command arguments.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tempfile
import time
from urllib.parse import unquote

from qualify_sip_registration import build_probe, port


class EventSocket:
    """Small bounded client for the native FreeSWITCH event socket protocol."""

    def __init__(self, number: int, password: str):
        self.socket = socket.create_connection(("127.0.0.1", number), timeout=3)
        self.file = self.socket.makefile("rb")
        self.events: list[dict[str, str]] = []
        header, _ = self.read()
        if header.get("content-type") != "auth/request":
            raise RuntimeError("FreeSWITCH did not request control authentication")
        self.socket.sendall(f"auth {password}\n\n".encode())
        header, _ = self.read()
        if not header.get("reply-text", "").startswith("+OK"):
            raise RuntimeError("FreeSWITCH control authentication failed")
        self.socket.sendall(b"event plain CUSTOM sofia::register sofia::unregister\n\n")
        self.read()

    def read(self) -> tuple[dict[str, str], str]:
        headers = {}
        size = 0
        while True:
            line = self.file.readline(8192)
            size += len(line)
            if not line or size > 65536:
                raise RuntimeError("Invalid or closed FreeSWITCH control response")
            if line in (b"\n", b"\r\n"):
                break
            key, separator, value = line.decode(errors="replace").partition(":")
            if separator:
                headers[key.lower()] = value.strip()
        length = int(headers.get("content-length", 0))
        if length > 1024 * 1024:
            raise RuntimeError("Oversized FreeSWITCH control response")
        return headers, self.file.read(length).decode(errors="replace") if length else ""

    def api(self, command: str) -> str:
        self.socket.sendall(f"api {command}\n\n".encode())
        for _ in range(100):
            headers, body = self.read()
            if headers.get("content-type") == "api/response":
                return body
            if headers.get("content-type") == "text/event-plain":
                fields = {}
                for line in body.splitlines():
                    key, separator, value = line.partition(":")
                    if separator:
                        fields[key.lower()] = unquote(value.strip())
                self.events.append({key: fields.get(key, "") for key in
                                    ("event-subclass", "profile-name", "from-user", "update-reg")})
        raise RuntimeError("No FreeSWITCH API response after bounded event drain")

    def close(self):
        self.file.close()
        self.socket.close()


def configuration(number_md5: int, number_sha: int, control: int, password: str) -> str:
    profiles = ""
    for name, number, algorithm in (("reg_md5", number_md5, "MD5"),
                                     ("reg_sha256", number_sha, "SHA-256")):
        profiles += f'''<profile name="{name}"><settings>
<param name="sip-ip" value="127.0.0.1"/><param name="rtp-ip" value="127.0.0.1"/>
<param name="sip-port" value="{number}"/><param name="dialplan" value="XML"/>
<param name="context" value="test"/><param name="auth-calls" value="true"/>
<param name="challenge-realm" value="127.0.0.1"/>
<param name="rfc8760-auth-algorithms" value="{algorithm}"/>
<param name="force-register-domain" value="127.0.0.1"/>
<param name="force-register-expires" value="60"/>
<param name="manage-presence" value="false"/><param name="tls" value="false"/>
<param name="inbound-codec-prefs" value="PCMU"/><param name="outbound-codec-prefs" value="PCMU"/>
</settings></profile>'''
    users = "".join(f'<user id="{user}"><params><param name="password" value="{password}"/></params></user>'
                    for user in ("7101", "7102"))
    return f'''<?xml version="1.0"?>
<document type="freeswitch/xml">
<section name="configuration">
<configuration name="modules.conf" description="Registration qualification">
<modules><load module="mod_commands"/><load module="mod_dptools"/>
<load module="mod_dialplan_xml"/><load module="mod_event_socket"/><load module="mod_sofia"/>
</modules></configuration>
<configuration name="switch.conf" description="Isolated core"><settings>
<param name="loglevel" value="WARNING"/><param name="rtp-start-port" value="39000"/>
<param name="rtp-end-port" value="39020"/></settings></configuration>
<configuration name="event_socket.conf" description="Loopback control"><settings>
<param name="listen-ip" value="127.0.0.1"/><param name="listen-port" value="{control}"/>
<param name="password" value="{password}"/><param name="nat-map" value="false"/>
</settings></configuration>
<configuration name="sofia.conf" description="Isolated registrars"><profiles>{profiles}</profiles></configuration>
</section>
<section name="directory"><domain name="127.0.0.1"><groups><group name="default"><users>{users}</users></group></groups></domain></section>
<section name="dialplan"><context name="test"/></section>
</document>
'''


def qualify(executable: Path, probe: Path, output: Path, refresh: bool = False):
    output.mkdir(parents=True, exist_ok=True)
    prefix = executable.parent.parent
    library = prefix / "lib"
    module_candidates = (prefix / "mod", library / "freeswitch/mod")
    modules = next((candidate for candidate in module_candidates if (candidate / "mod_sofia.so").is_file()), None)
    if modules is None:
        raise RuntimeError("FreeSWITCH mod_sofia is required")
    env = {**os.environ, "LD_LIBRARY_PATH": str(library)}
    version = subprocess.run([str(executable), "-version"], env=env, capture_output=True, text=True, check=True)
    results = {"scope": "production REGISTER engine, native FreeSWITCH, loopback sockets",
               "complete_esp_call_qualified": False, "freeswitch": version.stdout.strip(),
               "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(), "scenarios": []}
    password = secrets.token_hex(24)
    number_md5, number_sha, control_port = port(), port(), port()
    with tempfile.TemporaryDirectory(prefix="voip-freeswitch-") as temporary:
        root = Path(temporary)
        for directory in ("conf", "run", "log", "db", "storage", "cache"):
            (root / directory).mkdir()
        config = root / "conf/freeswitch.xml"
        config.write_text(configuration(number_md5, number_sha, control_port, password))
        config.chmod(0o600)
        capture = root / "pbx.log"
        client = None
        child = None
        with capture.open("wb") as log:
            process = subprocess.Popen([str(executable), "-nf", "-nc", "-nonat", "-nocal",
                "-base", str(root), "-conf", str(root / "conf"), "-log", str(root / "log"),
                "-run", str(root / "run"), "-db", str(root / "db"), "-mod", str(modules)],
                env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            try:
                until = time.monotonic() + 20
                while True:
                    if process.poll() is not None:
                        raise RuntimeError("Isolated FreeSWITCH exited before startup")
                    try:
                        client = EventSocket(control_port, password)
                        status = client.api("sofia status")
                        if "reg_md5" in status and "reg_sha256" in status:
                            break
                        client.close()
                        client = None
                    except (OSError, RuntimeError):
                        if client:
                            client.close()
                            client = None
                    if time.monotonic() >= until:
                        raise RuntimeError("FreeSWITCH registrar profiles did not become ready")
                    time.sleep(0.1)
                for transport in ("udp", "tcp"):
                    for user, algorithm, profile, number in (("7101", "MD5", "reg_md5", number_md5),
                            ("7102", "SHA-256", "reg_sha256", number_sha)):
                        before = len(client.events)
                        hold = 52 if refresh and transport == "udp" and algorithm == "MD5" else 3
                        child = subprocess.Popen([str(probe), transport, "127.0.0.1", str(number), user,
                            "127.0.0.1", str(hold)], env={**os.environ, "REG_TEST_PASSWORD": password},
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                        binding = ""
                        until = time.monotonic() + 10
                        while child.poll() is None and time.monotonic() < until:
                            status = client.api(f"sofia status profile {profile} reg")
                            if f"{user}@127.0.0.1" in status:
                                binding = status
                                break
                            time.sleep(0.1)
                        stdout, stderr = child.communicate(timeout=hold + 45)
                        remaining = client.api(f"sofia status profile {profile} reg")
                        events = [event for event in client.events[before:] if event["from-user"] == user]
                        renewals = sum(event["event-subclass"] == "sofia::register" for event in events)
                        record = {"transport": transport, "algorithm": algorithm, "hold_seconds": hold,
                                  "exit_code": child.returncode, "events": stdout.replace(password, "[redacted]"),
                                  "stderr": stderr.replace(password, "[redacted]"), "binding_observed": bool(binding),
                                  "registration_events": renewals, "unregistered": f"{user}@127.0.0.1" not in remaining}
                        results["scenarios"].append(record)
                        if child.returncode or not record["binding_observed"] or not record["unregistered"]:
                            raise RuntimeError(f"FreeSWITCH qualification failed for {transport}/{algorithm}")
                        if hold > 3 and renewals < 2:
                            raise RuntimeError("Independent FreeSWITCH registration refresh event was not observed")
                results["passed"] = True
            finally:
                if child is not None and child.poll() is None:
                    child.kill()
                    child.communicate()
                if client is not None:
                    try:
                        client.api("fsctl shutdown now")
                    except (OSError, RuntimeError):
                        pass
                    client.close()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                results["pbx_stopped"] = process.poll() is not None
                (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
                (output / "freeswitch.log").write_text(capture.read_text(errors="replace").replace(password, "[redacted]"))
    print(json.dumps({"passed": results.get("passed", False), "scenarios": len(results["scenarios"]),
                      "pbx_stopped": results["pbx_stopped"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeswitch", type=Path, default=Path(shutil.which("freeswitch") or
                        Path.home() / ".cache/voip-stack/freeswitch-1.11.3/bin/freeswitch"))
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    binary = args.probe.resolve() if args.probe else build_probe(args.output.resolve() / "native")
    qualify(args.freeswitch.resolve(), binary, args.output.resolve(), args.refresh)
