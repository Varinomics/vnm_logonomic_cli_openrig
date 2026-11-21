"""Check the real adapter against SDK launcher contracts and OpenRig root help."""

import copy
import json
import os
from pathlib import Path
import queue
import shlex
import subprocess
import sys
import tempfile
import threading


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


class Adapter:
    def __init__(self, executable):
        self.errors = tempfile.TemporaryFile()
        environment = dict(os.environ, OPENRIG_TEST_AMBIENT_SECRET="not-for-probes")
        self.process = subprocess.Popen(
            [executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.errors, env=environment)
        self.frames = queue.Queue()
        self.reader = threading.Thread(target=self.read_frames, daemon=True)
        self.reader.start()
        self.request_number = 0
        try:
            self.send({
                "type": "hello", "core_protocol_generations": [1],
                "package_id": "logonomic.cli.openrig",
                "generation_digest": "a" * 64, "generation_nonce": "b" * 32,
                "limits": {
                    "max_frame_bytes": 4194304, "max_json_depth": 32,
                    "max_in_flight_requests": 64, "max_progress_frames_per_request": 1000,
                },
                "host": {"product": "logonomic", "version": "contract-test"},
            })
            hello = self.receive()
            check(hello["type"] == "hello", "adapter handshake")
            check(hello["package_id"] == "logonomic.cli.openrig", "package identity")
            check(hello["generation_digest"] == "a" * 64, "package generation binding")
            check(hello["generation_nonce"] == "b" * 32, "package generation nonce")
            capabilities = self.receive()
            check(capabilities["type"] == "capabilities", "capability negotiation")
            pairs = {(item["capability_id"], item["version"])
                     for item in capabilities["capabilities"]}
            check(pairs == {
                ("presentation", 1), ("launcher-discovery", 1), ("launch-preparation", 3),
            }, "only implemented launcher capabilities are advertised")
        except Exception:
            self.dispose()
            raise

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        try:
            if exception_type is None:
                self.send({"type": "shutdown", "grace_ms": 3000})
                self.process.stdin.close()
                check(self.process.wait(timeout=5) == 0, "clean adapter shutdown")
                self.errors.seek(0)
                errors = self.errors.read().decode("utf-8", errors="replace")
                check(not errors, f"unexpected adapter diagnostics: {errors}")
        finally:
            self.dispose()

    def dispose(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.errors.close()

    def send(self, value):
        frame = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.process.stdin.write(frame + b"\n")
        self.process.stdin.flush()

    def read_frames(self):
        while True:
            frame = self.process.stdout.readline(4194305)
            self.frames.put(frame)
            if not frame or not frame.endswith(b"\n"):
                return

    def receive(self):
        try:
            frame = self.frames.get(timeout=10)
        except queue.Empty:
            raise RuntimeError("adapter response timeout") from None
        check(bool(frame), "unexpected adapter EOF")
        check(len(frame) <= 4194304, "wire frame exceeds the negotiated bound")
        check(frame.endswith(b"\n") and not frame.endswith(b"\r\n"), "wire delimiter must be LF")
        return json.loads(frame.decode("utf-8"))

    def invoke(self, capability, version, operation, payload):
        self.request_number += 1
        request_id = f"r{self.request_number}"
        self.send({
            "type": "invoke", "request_id": request_id,
            "capability_id": capability, "version": version,
            "operation": operation, "payload": payload, "deadline_ms": 5000,
        })
        while True:
            reply = self.receive()
            check(reply["request_id"] == request_id, "request correlation")
            if reply["type"] == "result":
                return reply


def successful(reply):
    check(reply.get("status") == "success", f"operation failed: {reply}")
    return reply["payload"]


def launcher(script, runner="direct", environment=""):
    return {"script_body": script, "runner_key": runner, "environment_text": environment}


def write_cli_fixture(program, help_text, directory, exit_code=0):
    # Root help is the native source-confirmed identity probe. A bare invocation
    # would open the dashboard, so the fixture records and rejects any other argv.
    program.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' \"$*\" >> {shlex.quote(str(directory / 'probe-arguments'))}\n"
        '[ "$#" = 1 ] && [ "$1" = --help ] || exit 71\n'
        '[ -z "${OPENRIG_TEST_AMBIENT_SECRET+x}" ] || exit 72\n'
        f'[ "$XDG_CONFIG_HOME" = {shlex.quote(str(directory / "config"))} ] || exit 73\n'
        f"printf '%s' {shlex.quote(help_text)}\n"
        f"exit {exit_code}\n",
        encoding="utf-8")
    program.chmod(0o755)


def check_discovery(adapter, directory, platform):
    program = directory / "rig"
    ambient = {"PATH": str(directory), "XDG_CONFIG_HOME": str(directory / "config")}
    request = {
        "ambient_environment": ambient, "default_home_fallback": str(directory),
        "platform": platform,
    }
    help_text = "Usage: rig [options] [command]\n\nCLI for the OpenRig local control plane\n\nOptions:\n"
    write_cli_fixture(program, help_text, directory)
    result = successful(adapter.invoke("launcher-discovery", 1, "discover", request))
    check(len(result["candidates"]) == 1, "source-confirmed OpenRig help is discovered")
    candidate = result["candidates"][0]
    check(candidate["script_body"] == "rig", "discovery preserves the dashboard command")
    check(candidate["display_name"] == "OpenRig", "discovery presentation")
    check(candidate["default"], "discovered launcher is the default candidate")

    for wrong_help in (
        "rig 0.7.4\nThe R Installation Manager\n",
        "Usage: rig [options]\n\nThe R Installation Manager\n\nCommands:\n  OpenRig example\n",
    ):
        write_cli_fixture(program, wrong_help, directory)
        result = successful(adapter.invoke("launcher-discovery", 1, "discover", request))
        check(result["candidates"] == [], "an unrelated rig must not be auto-discovered")

    write_cli_fixture(program, help_text, directory, exit_code=1)
    result = successful(adapter.invoke("launcher-discovery", 1, "discover", request))
    check(result["candidates"] == [], "a failed help probe cannot identify OpenRig")

    request["platform"] = "windows-x86_64"
    result = successful(adapter.invoke("launcher-discovery", 1, "discover", request))
    check(result["candidates"] == [], "Windows discovery requires a supported WSL bridge")
    check((directory / "probe-arguments").read_text().splitlines() == ["--help"] * 4,
          "discovery probes only root help, never a dashboard or seat action")

    program.unlink()
    request["platform"] = platform
    result = successful(adapter.invoke("launcher-discovery", 1, "discover", request))
    check(result["candidates"] == [], "missing executable discovery")


def check_recognition(adapter, directory, platform):
    for script, runner, expected in (
        ("rig", "direct", True),
        ("rig attach author", "direct", True),
        ('rig up "spec with spaces.json"', "direct", True),
        ('"/opt/open rig/rig" ps', "direct", True),
        ("rig", "bash", True),
        ("other-cli", "direct", False),
        ("rigctl", "direct", False),
        ("rig && echo changed", "bash", False),
        ("rig $(echo injected)", "bash", False),
    ):
        result = successful(adapter.invoke("launcher-discovery", 1, "recognize", {
            "launcher": launcher(script, runner), "platform": platform,
            "ambient_environment": {"PATH": str(directory)},
        }))
        check(result["recognized"] == expected, f"literal launcher recognition: {script}")
    result = successful(adapter.invoke("launcher-discovery", 1, "recognize", {
        "launcher": launcher("wsl rig"), "platform": "windows-x86_64", "ambient_environment": {},
    }))
    check(not result["recognized"], "unverified WSL invocations are not recognized")


def check_preparation(adapter, directory, platform):
    base = {
        "launcher": launcher('rig up "team spec.json"', environment="OPENRIG_HOME=/chosen/profile"),
        "launcher_argv": ["rig", "up", "team spec.json"],
        "launch_environment": {
            "platform": platform,
            "effective_environment": {"PATH": str(directory), "OPENRIG_HOME": "/chosen/profile"},
        },
        "working_directory": str(directory), "title_instruction": "", "declared_assets": [],
        "profile_storage": {"version": 1, "root": str(directory / "addon-profile")},
        "account_context": {
            "version": 2, "adapter_account_key": "selected-account",
            "profile_path": str(directory / "selected-profile"),
        },
    }
    for script, argv in (
        ("rig", ["rig"]),
        ("rig attach author", ["rig", "attach", "author"]),
        (base["launcher"]["script_body"], base["launcher_argv"]),
        ("rig", ["another-program"]),
    ):
        request = copy.deepcopy(base)
        request["launcher"]["script_body"] = script
        request["launcher_argv"] = argv
        result = successful(adapter.invoke("launch-preparation", 3, "prepare", request))
        check(result["disposition"] == {
            "version": 1, "prepared": False, "lifecycle_events": False, "coordination_delivery": False,
        }, "a dashboard credential cannot imply native-seat enrollment")
        check(result["environment"] == {"set": {}, "unset": []}, "preserve selected environment")
        check(result["working_directory"] == "inherit", "preserve working directory")
        check("argv" not in result, "preserve the exact selected shell and arguments")
        check(not result.get("apply_managed_configuration", False), "do not rewrite existing rig settings")
        check(result["profile_storage"] == base["profile_storage"], "preserve host profile context")
        check(result["account_context"] == base["account_context"], "preserve selected account context")

    request = copy.deepcopy(base)
    request["initial_prompt"] = {
        "version": 1, "text": "Read the handoff file.", "referenced_file": str(directory / "handoff.txt"),
    }
    result = adapter.invoke("launch-preparation", 3, "prepare", request)
    check(result["status"] == "unavailable" and result["reason"] == "initial_prompt_unavailable",
          "unimplemented model-context delivery is refused rather than silently dropped")


def main():
    with Adapter(sys.argv[1]) as adapter, tempfile.TemporaryDirectory(prefix="openrig-launchers-") as root:
        directory = Path(root)
        platform = sys.argv[2]
        check_discovery(adapter, directory, platform)
        check_recognition(adapter, directory, platform)
        check_preparation(adapter, directory, platform)
        invalid = adapter.invoke("launcher-discovery", 1, "recognize", {"launcher": {}})
        check(invalid["status"] == "error", "malformed request rejected")
        unknown = adapter.invoke("launch-preparation", 3, "unsupported_operation", {})
        check(unknown["status"] == "unavailable", "unknown operation rejected")
    print("PASS OpenRig launcher protocol: discovery, isolation, recognition and unchanged preparation")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
