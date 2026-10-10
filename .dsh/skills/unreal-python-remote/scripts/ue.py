#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ue.py -- drive a running Unreal Editor through the Python Editor Script Plugin.

Talks the engine's own remote-execution protocol (UDP multicast discovery on
239.0.0.1:6766 + TCP command channel), so the editor needs no extra plugin,
no open port forwarding and no code in the project. The protocol client that
ships with the engine (``remote_execution.py``) is loaded straight out of the
engine install, so this wrapper never drifts from the engine's wire format.

Usage (run ``ue.py --help`` for the full list)::

    ue.py doctor                       # is anything reachable at all?
    ue.py nodes                        # list running editors
    ue.py run -f C:\\temp\\job.py        # execute a script inside the editor
    ue.py run -c "print(unreal.SystemLibrary.get_engine_version())"
    ue.py eval "unreal.EditorLevelLibrary.get_all_level_actors()"

Exit codes: 0 ok, 1 the remote command failed, 2 could not reach an editor,
3 timed out, 4 bad usage.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

EXIT_OK = 0
EXIT_REMOTE_FAIL = 1
EXIT_NO_EDITOR = 2
EXIT_TIMEOUT = 3
EXIT_USAGE = 4

REMOTE_EXEC_RELPATH = Path(
    "Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/remote_execution.py"
)

DEFAULT_MULTICAST = "239.0.0.1"
DEFAULT_MULTICAST_PORT = 6766

MODE_FILE = "ExecuteFile"
MODE_STATEMENT = "ExecuteStatement"
MODE_EVAL = "EvaluateStatement"
MODES = {
    "file": MODE_FILE,
    "ExecuteFile": MODE_FILE,
    "statement": MODE_STATEMENT,
    "ExecuteStatement": MODE_STATEMENT,
    "eval": MODE_EVAL,
    "EvaluateStatement": MODE_EVAL,
}


# --------------------------------------------------------------------------- #
# console plumbing
# --------------------------------------------------------------------------- #

def _force_utf8() -> None:
    """UE logs routinely contain CJK text; never die on a cp936 console."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def _say(message: str) -> None:
    sys.stderr.write(message + "\n")
    sys.stderr.flush()


def _read_stdin_text() -> str:
    """Read piped source code without trusting the console code page.

    On Windows a shell may pipe UTF-8, UTF-16LE, or the ANSI code page. Decoding
    through ``sys.stdin`` yields lone surrogates for the mismatched cases, which
    then blow up when the protocol re-encodes the command as UTF-8.
    """
    stream = getattr(sys.stdin, "buffer", None)
    if stream is None:
        return sys.stdin.read()
    raw = stream.read()

    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le", errors="replace")
    if raw.startswith(b"\xfe\xff"):
        return raw[2:].decode("utf-16-be", errors="replace")
    # UTF-16 without a BOM shows up as NUL bytes in every other position.
    if raw and raw.count(b"\x00") > len(raw) // 4:
        try:
            return raw.decode("utf-16-le")
        except UnicodeDecodeError:
            pass
    try:
        return raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        import locale

        return raw.decode(locale.getpreferredencoding(False), errors="replace")


# --------------------------------------------------------------------------- #
# locating the engine
# --------------------------------------------------------------------------- #

def find_uproject(start: Path | None = None, explicit: str | None = None) -> Path | None:
    """Nearest ``*.uproject`` at or above *start* (default cwd)."""
    if explicit:
        candidate = Path(explicit).expanduser()
        if candidate.is_file() and candidate.suffix.lower() == ".uproject":
            return candidate.resolve()
        if candidate.is_dir():
            found = sorted(candidate.glob("*.uproject"))
            if found:
                return found[0].resolve()
        return None

    current = (start or Path.cwd()).resolve()
    for directory in (current, *current.parents):
        found = sorted(directory.glob("*.uproject"))
        if found:
            return found[0].resolve()
    return None


def _registry_engines() -> dict:
    """``{version: install_dir}`` from the Epic launcher's registry keys."""
    try:
        import winreg
    except ImportError:
        return {}

    engines: dict = {}
    for hive, path in (
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\EpicGames\Unreal Engine"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\EpicGames\Unreal Engine"),
    ):
        try:
            with winreg.OpenKey(hive, path, 0, winreg.KEY_READ) as key:
                index = 0
                while True:
                    try:
                        name = winreg.EnumKey(key, index)
                    except OSError:
                        break
                    index += 1
                    try:
                        with winreg.OpenKey(key, name) as sub:
                            install = winreg.QueryValueEx(sub, "InstalledDirectory")[0]
                    except OSError:
                        continue
                    engines.setdefault(name, install)
        except OSError:
            continue

    # Custom/source builds are registered by GUID under HKCU.
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, r"SOFTWARE\Epic Games\Unreal Engine\Builds", 0, winreg.KEY_READ
        ) as key:
            index = 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(key, index)
                except OSError:
                    break
                index += 1
                engines.setdefault(name, value)
    except OSError:
        pass
    return engines


def _engine_version_from_uproject(uproject: Path | None) -> str | None:
    if not uproject:
        return None
    try:
        data = json.loads(uproject.read_text(encoding="utf-8-sig"))
    except Exception:
        return None
    return data.get("EngineAssociation") or None


def find_engine(uproject: Path | None = None, explicit: str | None = None) -> Path | None:
    """Resolve the UE install root (the directory holding ``Engine/``)."""
    candidates: list[Path] = []

    def _add(value) -> None:
        if not value:
            return
        path = Path(str(value).strip().strip('"')).expanduser()
        # Accept being handed either the engine root or remote_execution.py itself.
        if path.is_file():
            path = path.parent
            for _ in range(4):
                if (path / "Engine").is_dir():
                    break
                path = path.parent
        elif (path / "Engine" / "Plugins").is_dir():
            pass
        elif (path / "Plugins" / "Experimental" / "PythonScriptPlugin").is_dir():
            # Someone pointed at <install>/Engine
            path = path.parent
        candidates.append(path)

    _add(explicit)
    for var in ("UE_ENGINE_DIR", "UNREAL_ENGINE_DIR", "UE_ROOT", "UE5_ENGINE"):
        _add(os.environ.get(var))

    engines = _registry_engines()
    version = _engine_version_from_uproject(uproject)
    if version and version in engines:
        _add(engines[version])
    if explicit is None and not any(os.environ.get(v) for v in ("UE_ENGINE_DIR", "UNREAL_ENGINE_DIR", "UE_ROOT", "UE5_ENGINE")):
        for name, install in sorted(engines.items(), reverse=True):
            _add(install)

    for root in ("C:/Program Files/Epic Games", "D:/Epic Games", "E:/Epic Games", "F:/Epic Games",
                 "C:/UnrealEngine", "D:/UnrealEngine", "E:/UnrealEngine", "F:/UnrealEngine"):
        base = Path(root)
        if not base.is_dir():
            continue
        try:
            for child in sorted(base.iterdir(), reverse=True):
                if child.is_dir() and re.match(r"^UE_?\d", child.name, re.I):
                    _add(child)
        except OSError:
            continue

    seen: set = set()
    for path in candidates:
        try:
            resolved = path.resolve()
        except OSError:
            continue
        if resolved in seen:
            continue
        seen.add(resolved)
        if (resolved / REMOTE_EXEC_RELPATH).is_file():
            return resolved
    return None


def load_remote_execution(engine_root: Path):
    """Import the engine's own protocol client."""
    python_dir = str(engine_root / REMOTE_EXEC_RELPATH.parent)
    if python_dir not in sys.path:
        sys.path.insert(0, python_dir)
    import remote_execution  # type: ignore

    return remote_execution


# --------------------------------------------------------------------------- #
# talking to the editor
# --------------------------------------------------------------------------- #

def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def make_session(remote_execution, multicast: str, multicast_port: int, command_port: int | None):
    config = remote_execution.RemoteExecutionConfig()
    config.multicast_ttl = 0
    config.multicast_group_endpoint = (multicast, multicast_port)
    config.multicast_bind_address = "127.0.0.1"
    config.command_endpoint = ("127.0.0.1", command_port or _free_port())
    return remote_execution.RemoteExecution(config), config


def discover(session, timeout: float) -> list:
    """Start discovery and wait for at least one ``pong`` (or *timeout*)."""
    session.start()
    deadline = time.time() + timeout
    while time.time() < deadline:
        nodes = session.remote_nodes
        if nodes:
            # Give a second pong cycle a chance so a second editor shows up too.
            time.sleep(0.4)
            return session.remote_nodes
        time.sleep(0.05)
    return session.remote_nodes


def select_node(nodes: list, selector: str | None):
    """Resolve ``--node``: exact id, id prefix, project/machine substring, or 1-based index."""
    if not nodes:
        return None
    if not selector:
        return nodes[0] if len(nodes) == 1 else None

    for node in nodes:
        if node.get("node_id") == selector:
            return node
    for node in nodes:
        if str(node.get("node_id", "")).startswith(selector):
            return node
    if selector.isdigit() and 1 <= int(selector) <= len(nodes):
        return nodes[int(selector) - 1]
    lowered = selector.lower()
    for node in nodes:
        haystack = " ".join(
            str(node.get(key, "")) for key in ("project_name", "project", "machine", "engine_version", "engine_root")
        ).lower()
        if lowered in haystack:
            return node
    return None


# --------------------------------------------------------------------------- #
# composing the command the engine will see
# --------------------------------------------------------------------------- #

STAGE_DIR_NAME = "ue_remote_agent"


def _stage_code(code: str) -> Path:
    """Write inline code to a real .py file so the editor can load it itself.

    Sending source *text* in ExecuteFile mode is unsafe. The engine scans the
    command for the literal substring ``.py`` and treats everything up to and
    including it as a file path (PythonScriptPlugin.cpp,
    ``TryExtractPathnameAndCommand``). A single comment like ``# see job.py``
    is then enough to turn a valid script into "Could not load Python file".
    Handing over a path sidesteps the whole heuristic and also gives tracebacks
    real filenames instead of ``<string>``.
    """
    directory = Path(tempfile.gettempdir()) / STAGE_DIR_NAME
    directory.mkdir(parents=True, exist_ok=True)
    handle, name = tempfile.mkstemp(prefix="uejob_", suffix=".py", dir=str(directory))
    with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(code)
    return Path(name)


def build_command(args) -> tuple:
    """Return ``(command_for_engine, source_label)``."""
    if args.mode != MODE_FILE:
        # ExecuteStatement / EvaluateStatement go straight to Python: no path sniffing.
        if args.source_code is None:
            _say("[ue] internal error: no source for a non-file execution mode")
            raise SystemExit(EXIT_USAGE)
        return args.source_code, "inline"

    if args.source_path is not None:
        path = args.source_path
    else:
        path = _stage_code(args.source_code or "")
    # Quote it: the engine strips a leading quote and requires the matching trailing one.
    return f'"{path}"', str(path)


def describe_node(node: dict) -> str:
    bits = []
    for label, key in (("project", "project_name"), ("engine", "engine_version"),
                       ("machine", "machine"), ("root", "engine_root")):
        if node.get(key):
            bits.append(f"{label}={node[key]}")
    bits.append(f"id={str(node.get('node_id', ''))[:8]}")
    return " ".join(bits)


def run_one_shot(args) -> int:
    uproject = find_uproject(explicit=args.uproject)
    engine = find_engine(uproject=uproject, explicit=args.engine)
    if engine is None:
        _say(
            "[ue] could not locate a Unreal Engine install.\n"
            "     Pass --engine <path to UE_5.x>, or set UE_ENGINE_DIR.\n"
            f"     Expected: <engine>/{REMOTE_EXEC_RELPATH.as_posix()}"
        )
        return EXIT_NO_EDITOR

    try:
        remote_execution = load_remote_execution(engine)
    except Exception as exc:  # pragma: no cover - broken install
        _say(f"[ue] failed to import remote_execution.py from {engine}: {exc}")
        return EXIT_NO_EDITOR

    remote_execution.set_log_level(50)  # silence the module's own debug chatter

    session, config = make_session(
        remote_execution, args.multicast, args.multicast_port, args.command_port
    )

    try:
        nodes = discover(session, args.connect_timeout)
    except Exception as exc:
        _say(f"[ue] discovery failed: {exc}")
        return EXIT_NO_EDITOR

    if not nodes:
        _say(
            "[ue] no Unreal Editor answered on the multicast group "
            f"{args.multicast}:{args.multicast_port}.\n"
            "     Check, in this order:\n"
            "       1. the editor is actually running;\n"
            "       2. Edit > Plugins > 'Python Editor Script Plugin' is enabled;\n"
            "       3. Project Settings > Plugins > Python > 'Enable Remote Execution' is on\n"
            "          (needs the editor to restart after toggling);\n"
            "       4. the multicast group / port match those Python settings."
        )
        return EXIT_NO_EDITOR

    if getattr(args, "list_only", False):
        payload = {"ok": True, "engine_root": str(engine), "nodes": nodes}
        if args.json:
            if args.command == "doctor":
                payload = {"python": sys.version.split()[0], "python_executable": sys.executable,
                           "uproject": str(uproject) if uproject else None, **payload}
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            if args.command == "doctor":
                print(f"python   : {sys.version.split()[0]}  ({sys.executable})")
                print(f"uproject : {uproject or '<not found>'}")
            print(f"[ue] engine: {engine}")
            for index, node in enumerate(nodes, 1):
                print(f"  [{index}] {describe_node(node)}")
        return EXIT_OK

    node = select_node(nodes, getattr(args, "node", None))
    if node is None:
        _say(f"[ue] {len(nodes)} editors are reachable; pick one with --node:")
        for index, candidate in enumerate(nodes, 1):
            _say(f"       {index}) {describe_node(candidate)}")
        return EXIT_USAGE

    try:
        session.open_command_connection(node["node_id"])
    except Exception as exc:
        _say(f"[ue] could not open a command channel to the editor: {exc}")
        return EXIT_NO_EDITOR

    command, source_note = build_command(args)
    exec_mode = args.mode
    try:
        result = session.run_command(command, unattended=not args.attended, exec_mode=exec_mode)
    except Exception as exc:
        _say(f"[ue] the editor dropped the command channel: {exc}")
        return EXIT_NO_EDITOR
    finally:
        try:
            session.stop()
        except Exception:
            pass

    success = bool(result.get("success"))
    output = result.get("output") or []
    detail = result.get("result") or ""

    if args.json:
        print(json.dumps(
            {
                "ok": success,
                "node": node,
                "source": source_note,
                "command": command,
                "exec_mode": exec_mode,
                "result": detail,
                "output": output,
            },
            ensure_ascii=False,
            indent=2,
        ))
    else:
        for entry in output:
            text = entry.get("output", "") if isinstance(entry, dict) else str(entry)
            kind = entry.get("type") if isinstance(entry, dict) else None
            if kind == "error":
                sys.stdout.write("[error] " + text)
            elif kind == "warning":
                sys.stdout.write("[warn] " + text)
            else:
                sys.stdout.write(text)
            if not text.endswith("\n"):
                sys.stdout.write("\n")
        if detail and (not success or exec_mode == MODE_EVAL or str(detail) != "None"):
            sys.stdout.write(("[error] " if not success else "[result] ") + str(detail) + "\n")
        sys.stdout.flush()
        if not success:
            _say(f"[ue] the command failed inside the editor (node {describe_node(node)}).")

    return EXIT_OK if success else EXIT_REMOTE_FAIL


def guarded(args) -> int:
    """Run the protocol work under a watchdog: recv() on the command channel blocks forever."""
    box: dict = {}

    def worker() -> None:
        try:
            box["code"] = run_one_shot(args)
        except BaseException as exc:  # noqa: BLE001 - surfaced to the caller
            box["error"] = exc

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    thread.join(args.timeout)

    if thread.is_alive():
        _say(
            f"[ue] timed out after {args.timeout:g}s.\n"
            "     The editor is very likely still running the command (a timeout does NOT cancel it).\n"
            "     Usual cause: the script blocked the game thread -- a modal dialog, input(),\n"
            "     a synchronous asset load/compile, or an infinite loop."
        )
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(EXIT_TIMEOUT)

    if "error" in box:
        _say(f"[ue] {type(box['error']).__name__}: {box['error']}")
        return EXIT_NO_EDITOR
    return box.get("code", EXIT_OK)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def build_parser() -> argparse.ArgumentParser:
    class _Parser(argparse.ArgumentParser):
        """argparse exits 2 on a usage error, which collides with EXIT_NO_EDITOR."""

        def error(self, message):
            self.print_usage(sys.stderr)
            _say(f"{self.prog}: error: {message}")
            raise SystemExit(EXIT_USAGE)

    parser = _Parser(
        prog="ue.py",
        description="Control a running Unreal Editor over the Python plugin's remote execution protocol.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  ue.py doctor\n"
            "  ue.py run -f job.py\n"
            "  ue.py run -c \"print(unreal.SystemLibrary.get_engine_version())\"\n"
            "  ue.py eval \"len(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())\"\n"
            "  ue.py run --stdin < job.py\n"
        ),
    )
    parser.add_argument("--engine", help="UE install root (or its remote_execution.py); auto-detected by default")
    parser.add_argument("--uproject", help="path to the .uproject used to resolve the engine version")
    parser.add_argument("--multicast", default=DEFAULT_MULTICAST, help=f"multicast group (default {DEFAULT_MULTICAST})")
    parser.add_argument("--multicast-port", type=int, default=DEFAULT_MULTICAST_PORT, help=f"multicast port (default {DEFAULT_MULTICAST_PORT})")
    parser.add_argument("--command-port", type=int, default=None, help="local TCP port the editor dials back to (default: any free port)")
    parser.add_argument("--connect-timeout", type=float, default=15.0, help="seconds to wait for editors to answer (default 15)")
    parser.add_argument("--timeout", type=float, default=120.0, help="hard wall-clock limit for the whole call (default 120)")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    sub = parser.add_subparsers(dest="command", required=True)

    # SUPPRESS keeps a subcommand from clobbering a --json given before it.
    inline_json = {"action": "store_true", "default": argparse.SUPPRESS, "help": "emit machine-readable JSON"}

    sub.add_parser("nodes", help="list reachable editor instances").add_argument("--json", **inline_json)
    sub.add_parser("doctor", help="report python/engine/uproject resolution and reachable editors").add_argument("--json", **inline_json)

    run = sub.add_parser("run", help="execute Python inside the editor")
    run.add_argument("-f", "--file", dest="script_file", help="run this .py file (UTF-8)")
    run.add_argument("-c", "--code", help="run this code string")
    run.add_argument("--stdin", action="store_true", help="read the code from stdin")
    run.add_argument("--mode", default="file", choices=sorted(MODES), help="engine execution mode (default file)")
    run.add_argument("--node", help="which editor: id, id prefix, project/machine substring, or 1-based index")
    run.add_argument("--attended", action="store_true", help="allow UI (default is unattended: modal dialogs are suppressed)")
    run.add_argument("--json", **inline_json)

    ev = sub.add_parser("eval", help="evaluate one expression and print its value")
    ev.add_argument("expression", help="Python expression")
    ev.add_argument("--node", help="which editor to talk to")
    ev.add_argument("--json", **inline_json)

    return parser


def main(argv=None) -> int:
    _force_utf8()
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.command == "run":
        sources = [bool(args.script_file), args.code is not None, bool(args.stdin)]
        if sum(sources) != 1:
            parser.error("run needs exactly one of --file / --code / --stdin")
        args.source_path = None
        args.source_code = None
        if args.script_file:
            candidate = Path(args.script_file).expanduser()
            if not candidate.is_file():
                _say(f"[ue] cannot read {args.script_file}: no such file")
                return EXIT_USAGE
            args.source_path = candidate.resolve()
        elif args.stdin:
            args.source_code = _read_stdin_text()
        else:
            args.source_code = args.code
        args.mode = MODES[args.mode]

    elif args.command == "eval":
        args.source_path = None
        args.source_code = args.expression
        args.mode = MODE_EVAL
        args.attended = False

    elif args.command == "nodes":
        args.list_only = True

    elif args.command == "doctor":
        args.list_only = True

    return guarded(args)


if __name__ == "__main__":
    sys.exit(main())
