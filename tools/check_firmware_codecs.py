"""Check the linked ESP firmware, using the toolchain recorded by CMake."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


G722_FUNCTIONS = {
    "esp_g722_enc_open", "esp_g722_enc_process",
    "esp_g722_dec_open", "esp_g722_dec_decode",
}
MACHINES = {"esp32s3": 94, "esp32p4": 243}


def inspect_build(project: Path, *, target: str, expect_g722: bool) -> dict:
    elf = project / "build/firmware.elf"
    defines = (project / "src/esphome/core/defines.h").read_text()
    with elf.open("rb") as source:
        header = source.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected a linked little-endian 32-bit ESP ELF")
    elf_type, machine = struct.unpack("<HH", header[16:20])
    if elf_type not in (2, 3) or machine != MACHINES[target]:
        raise ValueError("Firmware ELF architecture does not match its gate target")
    if not re.search(r"^#define USE_ESPHOME_VOIP_STACK_PCM\b", defines, re.MULTILINE):
        raise ValueError("Expected the baseline PCM implementation in this firmware gate")
    enabled = bool(re.search(r"^#define USE_ESPHOME_VOIP_STACK_G722\b", defines, re.MULTILINE))
    if enabled != expect_g722:
        raise ValueError("Generated G722 define differs from the expected codec gate")
    if expect_g722:
        for direction in ("MIC", "SPEAKER"):
            if not re.search(rf"^#define USE_ESPHOME_VOIP_STACK_{direction}\b", defines, re.MULTILINE):
                raise ValueError("G722 gate requires both microphone and speaker owners")
    cache = (project / "build/CMakeCache.txt").read_text()
    match = re.search(r"^CMAKE_NM:FILEPATH=(.+)$", cache, re.MULTILINE)
    if match is None or not Path(match[1].strip()).is_file():
        raise ValueError("The configured ESP toolchain nm is unavailable")
    nm = Path(match[1].strip())
    result = subprocess.run([str(nm), "--defined-only", "--format=posix", str(elf)],
                            capture_output=True, text=True, check=True)
    symbols = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2:
            symbols[fields[0]] = fields[1]
    linked = {name for name in G722_FUNCTIONS if symbols.get(name) in {"T", "t", "W", "w"}}
    if expect_g722 and linked != G722_FUNCTIONS:
        raise ValueError("Missing linked G722 encoder/decoder entry points: " + ", ".join(sorted(G722_FUNCTIONS - linked)))
    if not expect_g722 and any(name.startswith("esp_g722_") or "G722RtpCodec" in name for name in symbols):
        raise ValueError("PCM firmware unexpectedly contains linked G722 code")
    with elf.open("rb") as source:
        elf_digest = hashlib.file_digest(source, "sha256").hexdigest()
    return {"target": target, "g722": enabled, "elf": str(elf),
            "elf_sha256": elf_digest,
            "defines_sha256": hashlib.sha256(defines.encode()).hexdigest(),
            "nm": str(nm), "linked_g722_functions": sorted(linked)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--target", choices=MACHINES, required=True)
    parser.add_argument("--g722", choices=("enabled", "disabled"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    proof = inspect_build(args.project, target=args.target, expect_g722=args.g722 == "enabled")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(proof, indent=2) + "\n")


if __name__ == "__main__":
    main()
