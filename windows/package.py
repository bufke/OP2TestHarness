"""Packs a release: build/release/OP2TestHarness-<VERSION>.zip and its .sha256.

  python -I windows/package.py [--allow-dirty]

Run windows\\build.bat first. The zip holds one folder, OP2TestHarness-<VERSION>/, laid out like the repository so the
scripts work from it as they are:
  build/win/*.dll   harness.dll, zScenario.dll, zHarnessEnd.dll and the seven bots (built by windows\\build.bat)
  everything git tracks: the Python driver (run.py, shell.py, netgame.py, prefixes.py, tools/, windows/), the net/
  scripts, the self-tests, the sources and the docs
  SHA256SUMS        the SHA-256 of every other file in the folder (`sha256sum -c SHA256SUMS` checks them)
The zip is reproducible: entries are sorted and dated with the commit's time, so the same commit and the same DLLs give
the same zip. It refuses a working tree with uncommitted changes (they wouldn't match the commit) unless --allow-dirty.
"""
import argparse
import hashlib
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DLLS = ["harness.dll", "zScenario.dll", "zHarnessEnd.dll",
        *(f"{b}_bot.dll" for b in ("sample", "easter", "cursor", "hpblink", "hitmask", "los", "edge"))]


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--allow-dirty", action="store_true", help="pack even with uncommitted changes")
    a = ap.parse_args()
    version = (ROOT / "VERSION").read_text().strip()
    if git("status", "--porcelain", "--untracked-files=no").strip() and not a.allow_dirty:
        sys.exit("package: uncommitted changes; commit them first (or --allow-dirty)")
    commit = git("rev-parse", "--short", "HEAD").strip()
    stamp = time.gmtime(int(git("log", "-1", "--format=%ct").strip()))[:6]

    files = {}  # path inside the folder -> bytes
    for rel in git("ls-files", "-z").split("\0"):
        if rel:
            # git's stored form (LF), not the Windows checkout's CRLF, so the zip matches on every platform.
            files[rel] = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT, capture_output=True, check=True).stdout
    for dll in DLLS:
        p = ROOT / "build" / "win" / dll
        if not p.exists():
            sys.exit(f"package: {p} missing: run windows\\build.bat first")
        files[f"build/win/{dll}"] = p.read_bytes()
    sums = "".join(f"{hashlib.sha256(files[k]).hexdigest()}  {k}\n" for k in sorted(files))
    files["SHA256SUMS"] = sums.encode()

    name = f"OP2TestHarness-{version}"
    out = ROOT / "build" / "release"
    out.mkdir(parents=True, exist_ok=True)
    zpath = out / f"{name}.zip"
    executable = {"run.sh", "shell.sh"}
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in sorted(files):
            info = zipfile.ZipInfo(f"{name}/{rel}", date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100755 if rel in executable else 0o100644) << 16
            z.writestr(info, files[rel])
    digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
    (out / f"{name}.zip.sha256").write_text(f"{digest}  {zpath.name}\n")
    print(f"package: {zpath} ({len(files)} files, commit {commit})\n  sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
