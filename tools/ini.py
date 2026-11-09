#!/usr/bin/env python3
"""Register/unregister the Harness module in OPU/outpost2.ini, preserving everything else.

Backs the ini up once (outpost2.ini.pre-harness) before the first change. The module itself is inert unless the
OP2H_MISSION environment variable is set, so leaving it registered does not affect normal play.
"""
import re
import shutil
import sys
from pathlib import Path

ENTRY = "Harness=yes"
SECTION = "[Harness]\nDll=Harness\\harness.dll\n"


def install(text: str) -> str:
    if not re.search(r"(?mi)^Harness\s*=", text):
        if not re.search(r"(?mi)^\[ExternalModules\]", text):
            text = text.rstrip("\n") + "\n[ExternalModules]\n"
        text = re.sub(r"(?mi)^(\[ExternalModules\][^\n]*\n(?:(?!\[)[^\n]*\n)*?)(?=\[|\Z)",
                      lambda m: m.group(1).rstrip("\n") + "\n" + ENTRY + "\n", text, count=1)
    if not re.search(r"(?mi)^\[Harness\]", text):
        text = text.rstrip("\n") + "\n" + SECTION
    return text


def uninstall(text: str) -> str:
    text = re.sub(r"(?mi)^Harness\s*=[^\n]*\n?", "", text)
    return re.sub(r"(?mis)^\[Harness\]\n(?:(?!\[)[^\n]*\n?)*", "", text)


def main() -> None:
    action, path = sys.argv[1], Path(sys.argv[2])
    raw = path.read_bytes().decode("latin-1")
    eol = "\r\n" if "\r\n" in raw else "\n"
    text = raw.replace("\r\n", "\n")
    new = install(text) if action == "install" else uninstall(text)
    if new == text:
        print(f"{path.name}: already {action}ed")
        return
    backup = path.with_name(path.name + ".pre-harness")
    if not backup.exists():
        shutil.copy2(path, backup)
        print(f"backed up to {backup}")
    path.write_bytes(new.replace("\n", eol).encode("latin-1"))
    print(f"{path.name}: {action}ed")


if __name__ == "__main__":
    main()
