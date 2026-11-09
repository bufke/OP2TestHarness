"""Which Wine prefix the harness uses, and which prefixes it must never install into or run a private game in.

Shared by run.py and netgame.py (Makefile applies the same rules in make). The prefixes live beside this folder:
`../prefix` is the one you play from, `../prefix-harness` a copy of it for harness runs.
- OP2_HARNESS_PREFIX, when set, must be an absolute path: run.py would resolve a relative one against the current
  directory and the Makefile against this folder, so `../prefix` could pass a check and still install into prefix/.
- In a git checkout the play prefix is also the main checkout's `prefix` (the parent of the git common dir), so a
  worktree without a prefix link still knows it.
"""
import os
import subprocess
from pathlib import Path


def main_checkout(root: Path) -> Path:
    """The main checkout of `root`'s repository (`root` itself when it is the main one or git can't say)."""
    try:
        out = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=root,
                             capture_output=True, text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return Path(out.stdout.strip()).parent
    except (OSError, subprocess.SubprocessError):
        pass
    return root


def play_prefixes(root: Path) -> set:
    """The play prefix, resolved, as this folder and (in a git worktree) the main checkout name it."""
    return {(r / "prefix").resolve() for r in (root, main_checkout(root))}


def shared_prefixes(root: Path) -> set:
    """The play prefix and prefix-harness (every checkout's harness runs), resolved, of this and the main checkout."""
    return play_prefixes(root) | {(r / "prefix-harness").resolve() for r in (root, main_checkout(root))}


def env_prefix() -> Path | None:
    """OP2_HARNESS_PREFIX, or None; ValueError when it isn't an absolute path."""
    value = os.environ.get("OP2_HARNESS_PREFIX", "")
    if not value:
        return None
    if not os.path.isabs(value):
        raise ValueError(f"OP2_HARNESS_PREFIX must be an absolute path, not {value!r}")
    return Path(value)


def harness_prefix(root: Path) -> Path:
    """The prefix harness runs use: OP2_HARNESS_PREFIX, else prefix-harness/ when it exists, else prefix/ (run.py)."""
    env = env_prefix()
    if env is not None:
        return env
    own = root / "prefix-harness"
    return own if own.is_dir() else root / "prefix"
