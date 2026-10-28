#!/usr/bin/env python3
"""Qualify the production REGISTER engine against an isolated native Asterisk.

This checks registration bindings, not ESP media or the complete call FSM.
The PBX binds loopback only. Its generated credentials and configuration are
private and removed when the process exits. No system service is changed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import tempfile
import time


def port() -> int:
    with socket.socket() as stream, socket.socket(type=socket.SOCK_DGRAM) as udp:
        stream.bind(("127.0.0.1", 0))
        number = stream.getsockname()[1]
        udp.bind(("127.0.0.1", number))
        return number


def build_probe(output: Path) -> Path:
    """Build the maintained native driver using only a C++ compiler and OpenSSL."""
    root = Path(__file__).resolve().parents[1]
    include = output / "include/esphome/core/defines.h"
    include.parent.mkdir(parents=True, exist_ok=True)
    include.write_text("")
    binary = output / "registration-pbx-probe"
    source = root / "esphome/components/voip_stack"
    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-DUSE_ESPHOME_VOIP_REGISTRATION", "-DVOIP_REGISTRATION_CAPACITY=2",
               "-I", str(output / "include"), "-I", str(source),
               str(source / "sip_registration.cpp"),
               str(root / "tests/fixtures/registration_pbx_probe.cpp"), "-lcrypto", "-o", str(binary)]
    subprocess.run(command, check=True, capture_output=True, text=True)
    return binary


def qualify(probe: Path, output: Path, documentation: Path, refresh: bool = False) -> None:
    executable = shutil.which("asterisk")
    if not executable:
        raise RuntimeError("Native Asterisk is required")
    modules = Path("/usr/lib/asterisk/modules")
    if not (modules / "res_pjsip_registrar.so").is_file():
        raise RuntimeError("Asterisk PJSIP registrar modules are required")
    output.mkdir(parents=True, exist_ok=True)
    password = secrets.token_hex(24)
    number = port()
    results = {
        "scope": "production REGISTER engine, independent native PBX, loopback sockets",
        "complete_esp_call_qualified": False,
        "asterisk": subprocess.check_output([executable, "-V"], text=True).strip(),
        "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(),
        "scenarios": [],
    }
    with tempfile.TemporaryDirectory(prefix="voip-registration-") as temporary:
        root = Path(temporary)
        for directory in ("etc", "run", "var", "log", "spool", "agi", "keys", "data"):
            (root / directory).mkdir()
        # Asterisk needs its installed XML type definitions even in an isolated
        # configuration. The PBX state itself stays in the private directory.
        if not (documentation / "core-en_US.xml").is_file():
            raise RuntimeError("Installed Asterisk XML documentation is required")
        (root / "data/documentation").symlink_to(documentation, target_is_directory=True)
        directories = {
            "astetcdir": root / "etc", "astmoddir": modules,
            "astvarlibdir": root / "var", "astdbdir": root / "var",
            "astkeydir": root / "keys", "astdatadir": root / "data",
            "astagidir": root / "agi", "astspooldir": root / "spool",
            "astrundir": root / "run", "astlogdir": root / "log",
        }
        configuration = root / "asterisk.conf"
        configuration.write_text("[directories]\n" + "".join(
            f"{key} => {value}\n" for key, value in directories.items()
        ) + "[options]\nverbose=0\ndebug=0\n")
        needed = ("res_pjproject", "res_sorcery_config", "res_sorcery_memory",
                  "res_sorcery_astdb", "res_pjsip", "res_pjsip_authenticator_digest",
                  "res_pjsip_endpoint_identifier_user", "res_pjsip_registrar", "res_pjsip_logger")
        (root / "etc/modules.conf").write_text("[modules]\nautoload=no\n" + "".join(
            f"load={name}.so\n" for name in needed
        ))
        transports = "".join(
            f"[transport-{kind}]\ntype=transport\nprotocol={kind}\nbind=127.0.0.1:{number}\n"
            for kind in ("udp", "tcp")
        )
        accounts = ""
        for user, algorithm in (("7101", "MD5"), ("7102", "SHA-256")):
            accounts += (
                f"[{user}]\ntype=endpoint\ncontext=test\ndisallow=all\nallow=ulaw\n"
                f"auth={user}-auth\naors={user}\n"
                f"[{user}-auth]\ntype=auth\nauth_type=userpass\nusername={user}\n"
                f"password={password}\nsupported_algorithms_uas={algorithm}\n"
                f"[{user}]\ntype=aor\nmax_contacts=2\nremove_existing=yes\n"
                "minimum_expiration=1\ndefault_expiration=60\n"
            )
        pjsip = root / "etc/pjsip.conf"
        pjsip.write_text(transports + accounts)
        pjsip.chmod(0o600)
        capture = root / "asterisk-output.log"
        child = None
        with capture.open("wb") as log:
            process = subprocess.Popen([executable, "-f", "-C", str(configuration)],
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while True:
                    if process.poll() is not None:
                        raise RuntimeError("Isolated Asterisk exited before startup")
                    check = subprocess.run([executable, "-C", str(configuration), "-rx",
                                            "pjsip show transports"], capture_output=True, text=True)
                    if check.returncode == 0 and "transport-tcp" in check.stdout and "transport-udp" in check.stdout:
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Asterisk transports did not become ready")
                    time.sleep(0.1)
                buildopts = subprocess.run([executable, "-C", str(configuration), "-rx", "pjproject show buildopts"],
                                           capture_output=True, text=True, check=True)
                (output / "pjproject-buildopts.txt").write_text(buildopts.stdout)
                subprocess.run([executable, "-C", str(configuration), "-rx", "pjsip set logger on"],
                               capture_output=True, text=True, check=True)
                for transport in ("udp", "tcp"):
                    for user, algorithm in (("7101", "MD5"), ("7102", "SHA-256")):
                        env = {**os.environ, "REG_TEST_PASSWORD": password}
                        hold = 52 if refresh and transport == "udp" and algorithm == "MD5" else 3
                        try:
                            child = subprocess.Popen([str(probe), transport, "127.0.0.1", str(number),
                                                      user, "127.0.0.1", str(hold)],
                                                     env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                            binding = ""
                            until = time.monotonic() + 10
                            while child.poll() is None and time.monotonic() < until:
                                contacts = subprocess.run([executable, "-C", str(configuration), "-rx", "pjsip show contacts"],
                                                          capture_output=True, text=True, check=True)
                                if "sip:" in contacts.stdout and user in contacts.stdout:
                                    binding = contacts.stdout
                                    break
                                time.sleep(0.1)
                            stdout, stderr = child.communicate(timeout=hold + 45)
                            result = subprocess.CompletedProcess(child.args, child.returncode, stdout, stderr)
                        except subprocess.TimeoutExpired as error:
                            child.kill()
                            stdout, stderr = child.communicate()
                            stdout = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else error.stdout or ""
                            stderr = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else error.stderr or ""
                            results["scenarios"].append({"transport": transport, "algorithm": algorithm,
                                "exit_code": "timeout", "events": stdout.replace(password, "[redacted]"),
                                "stderr": stderr.replace(password, "[redacted]")})
                            raise RuntimeError(f"Registration probe timeout for {transport}/{algorithm}") from error
                        record = {"transport": transport, "algorithm": algorithm,
                                  "exit_code": result.returncode, "events": result.stdout,
                                  "stderr": result.stderr.replace(password, "[redacted]"),
                                  "binding_observed": bool(binding), "hold_seconds": hold}
                        results["scenarios"].append(record)
                        if result.returncode:
                            raise RuntimeError(f"Registration probe failed for {transport}/{algorithm}")
                        if not binding:
                            raise RuntimeError(f"No PBX binding observed for {transport}/{algorithm}")
                        if hold > 3 and result.stdout.count('"state":"registered"') < 2:
                            # A successful refresh retains REGISTERED. Its final
                            # status need not emit a duplicate state event; use
                            # the independent PBX packet trace below instead.
                            transcript = capture.read_text(errors="replace")
                            if len(set(re.findall(r"CSeq: (\d+) REGISTER", transcript))) < 4:
                                raise RuntimeError("Registration refresh was not observed")
                        contacts = subprocess.run([executable, "-C", str(configuration), "-rx",
                                                   "pjsip show contacts"], capture_output=True, text=True, check=True)
                        if "sip:" in contacts.stdout:
                            raise RuntimeError("A registration binding remained after unregister")
                results["passed"] = True
            finally:
                if child is not None and child.poll() is None:
                    child.kill()
                    child.communicate()
                if process.poll() is None:
                    try:
                        subprocess.run([executable, "-C", str(configuration), "-rx", "core stop now"],
                                       capture_output=True, timeout=5)
                    except (OSError, subprocess.TimeoutExpired):
                        process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                results["pbx_stopped"] = process.poll() is not None
                (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
                (output / "asterisk.log").write_text(capture.read_text(errors="replace").replace(password, "[redacted]"))
    print(json.dumps({"passed": results.get("passed", False), "scenarios": len(results["scenarios"]),
                      "pbx_stopped": results["pbx_stopped"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--documentation", type=Path, default=Path("/var/lib/asterisk/documentation"))
    parser.add_argument("--refresh", action="store_true", help="Also wait for a real UDP/MD5 registration refresh")
    args = parser.parse_args()
    probe = args.probe.resolve() if args.probe else build_probe(args.output.resolve() / "native")
    qualify(probe, args.output.resolve(), args.documentation.resolve(), args.refresh)
