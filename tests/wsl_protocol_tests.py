"""Exercise Windows WSL launcher admission and the saved native argument contract."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from launcher_protocol_tests import Adapter, check, launcher, successful


def recognize(adapter, script, environment):
    return successful(adapter.invoke("launcher-discovery", 1, "recognize", {
        "launcher": launcher(script),
        "platform": "windows-x86_64",
        "ambient_environment": environment,
    }))["recognized"]


def word(value):
    return '"' + value.replace('"', '"""') + '"'


def check_passthrough(adapter, source, argv, environment, directory):
    saved = launcher(source, environment="OPENRIG_HOME=/chosen/guest/profile")
    request = {
        "launcher": saved, "launcher_argv": argv, "working_directory": str(directory),
        "launch_environment": {"platform": "windows-x86_64", "effective_environment": environment},
        "title_instruction": "", "declared_assets": [],
        "profile_storage": {"version": 1, "root": str(directory / "profile")},
    }
    patch = successful(adapter.invoke("launch-preparation", 3, "prepare", request))
    check(patch["disposition"] == {"version": 1, "prepared": False, "lifecycle_events": False,
                                  "coordination_delivery": False}, "WSL passthrough cannot claim a managed seat")
    check("argv" not in patch and patch["working_directory"] == "inherit", "keep distro, user, cwd and literal arguments")
    check(patch["environment"] == {"set": {}, "unset": []}, "preserve selected environment")
    check(patch["profile_storage"] == request["profile_storage"], "preserve the Windows profile")
    request["initial_prompt"] = {"version": 1, "text": "Read the handoff.", "referenced_file": str(directory / "handoff.txt")}
    result = adapter.invoke("launch-preparation", 3, "prepare", request)
    check(result["status"] == "unavailable" and result["reason"] == "initial_prompt_unavailable",
          "an unmanaged dashboard cannot receive model instructions")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("adapter")
    parser.add_argument("--distro", help="exercise a running WSL2 distribution containing stock rig")
    parser.add_argument("--guest-rig", help="exact CLI path expected from discovery in that distribution")
    args = parser.parse_args()
    check(bool(args.distro) == bool(args.guest_rig), "select both the native distribution and installed CLI")
    names = ("PATH", "PATHEXT", "APPDATA", "LOCALAPPDATA", "USERPROFILE", "SystemRoot")
    environment = {key: os.environ[key] for key in names if key in os.environ}
    with tempfile.TemporaryDirectory(prefix="openrig wsl ") as raw, Adapter(args.adapter) as adapter:
        directory = Path(raw)
        for script in ("rig up spec.json", "wsl rig", "wsl.exe --exec rig",
                       "wsl.exe -d absent-openrig-test-distribution --exec rig",
                       "wsl.exe -d distro -d second --exec rig",
                       "wsl.exe --distribution distro --exec /bin/sh -c rig",
                       "wsl.exe --distribution distro --exec /usr/local/bin/rigctl",
                       "wsl.exe --distribution distro rig up spec.json"):
            check(not recognize(adapter, script, environment), f"unselected or ambiguous WSL launch: {script}")
        argv = ["wsl.exe", "--distribution", "a selected distro", "--user", "developer", "--cd",
                "/work dir", "--exec", "/tools/rig", "up", "spec with spaces.json"]
        check_passthrough(adapter, " ".join(word(arg) for arg in argv), argv, environment, directory)
        if not args.distro:
            return
        result = successful(adapter.invoke("launcher-discovery", 1, "discover", {
            "platform": "windows-x86_64", "ambient_environment": environment, "default_home_fallback": raw,
        }))
        subprocess_args = [str(Path(os.environ["SystemRoot"]) / "System32/wsl.exe"), "--distribution", args.distro,
                           "--exec", args.guest_rig]
        suffix = f" --distribution {word(args.distro)} --exec {word(args.guest_rig)}"
        selected = [item for item in result["candidates"] if item["script_body"].endswith(suffix)]
        check(len(selected) == 1 and selected[0]["runner_key"] == "direct", "found exact stock guest executable and distro")
        published_program = selected[0]["script_body"].removesuffix(suffix).strip('"')
        check(Path(published_program).samefile(subprocess_args[0]), "candidate uses the Windows system WSL executable")
        native_version = subprocess.run(subprocess_args + ["--version"], capture_output=True, text=True,
                                        check=True, timeout=20, env=environment).stdout.strip()
        check(bool(native_version), "stock OpenRig reports a native version")
        user = subprocess.check_output(subprocess_args[:3] + ["--exec", "/usr/bin/id", "-un"],
                                       text=True, timeout=10, env=environment).strip()
        selected_argv = subprocess_args[:3] + ["--user", user, "--cd", "/tmp"] + subprocess_args[3:] + ["help", "up"]
        selected_script = " ".join(word(arg) for arg in selected_argv)
        check(recognize(adapter, selected_script, environment), "saved exact distro, guest path, user and directory")
        check(not recognize(adapter, selected_script.replace("--exec", "--"), environment), "literal guest execution required")
        shadow = directory / "wsl.exe"
        shadow.write_bytes(b"unrelated executable")
        shadowed = dict(environment, PATH=str(directory) + os.pathsep + environment["PATH"])
        shadow_script = " ".join(word(arg) for arg in ["wsl.exe", *selected_argv[1:]])
        check(not recognize(adapter, shadow_script, shadowed), "reject a same-named WSL executable earlier on the selected PATH")
        check_passthrough(adapter, selected_script, selected_argv, environment, directory)
        native = subprocess.run(selected_argv, check=True, capture_output=True, text=True, timeout=20, env=environment)
        check("Usage: rig up" in native.stdout, "stock subcommand receives original arguments")
        print(json.dumps({"version": native_version, "distro": args.distro, "saved": selected_script,
                          "candidate": selected[0]["candidate_id"], "native_help": native.stdout.splitlines()[0]}))


if __name__ == "__main__":
    main()
