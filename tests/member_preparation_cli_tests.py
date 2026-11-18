"""Exercise the packaged authored-spec command against its adopted file contract."""

import copy
import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    program = pathlib.Path(sys.argv[1]).resolve(strict=True)
    prefix = ["/opt/seat tools/stable-launch", "installation-reference", "", "--"]
    previous = ["sandbox", "", "'literal'", "$(not executed)", "révision", "--"]
    original = {
        "version": "0.2",
        "name": "compiler-team",
        "env": {"CLAUDE_CONFIG_DIR": "./account", "EDITOR": "editor --wait"},
        "permission_policy": "builtin:standard",
        "extra": {"exact_integer": 9007199254740993, "fraction": 0.125, "flag": True},
        "pods": [{
            "id": "build",
            "members": [
                {"id": "worker", "runtime": "codex", "cwd": "./source with spaces",
                 "agent_ref": "local:./agents/build", "launch_wrapper": previous},
                {"id": "reviewer", "runtime": "claude-code", "launch_wrapper": ["other", "--"]},
            ],
        }],
    }

    with tempfile.TemporaryDirectory(prefix="openrig-preparation-") as temporary:
        root = pathlib.Path(temporary)
        directory = root / "authored specs"
        directory.mkdir()
        source = directory / "input.json"
        source_bytes = (json.dumps(original, ensure_ascii=False, indent=2) + "\n\n").encode()
        source.write_bytes(source_bytes)

        def command(output, *, input_path=source, expected=previous, wrapper=prefix, pod="build"):
            arguments = [
                str(program), "--spec", str(input_path), "--output", str(output),
                "--rig", "compiler-team", "--member", "worker",
                "--expect-wrapper", json.dumps(expected),
                "--wrapper-prefix", json.dumps(wrapper),
            ]
            if pod is not None:
                arguments.extend(["--pod", pod])
            return arguments

        def run(arguments, expected_code=0):
            result = subprocess.run(arguments, cwd=root, capture_output=True, timeout=10)
            assert result.returncode == expected_code, result.stderr.decode(errors="replace")
            assert result.stdout == b"", result.stdout
            if expected_code:
                assert result.stderr, "A refusal must explain the failed operation."
            else:
                assert result.stderr == b"", result.stderr
            assert source.read_bytes() == source_bytes, "The original spec bytes changed."
            assert not list(directory.glob(".openrig-member-*")), "Temporary output was retained."
            return result

        expected_document = copy.deepcopy(original)
        expected_document["pods"][0]["members"][0]["launch_wrapper"] = prefix + previous
        output = directory / "prepared.json"
        run(command(output))
        assert json.loads(output.read_bytes()) == expected_document

        # Reapplying the identical complete prefix preserves even the input's
        # formatting, and does not stack or absorb the inherited wrapper.
        repeated = directory / "repeated.json"
        run(command(repeated, input_path=output, expected=prefix + previous))
        assert repeated.read_bytes() == output.read_bytes()

        for name, expectation in [("absent", None), ("empty", []), ("different", ["changed"] )]:
            rejected = directory / (name + ".json")
            run(command(rejected, expected=expectation), 2)
            assert not rejected.exists()

        # A flat authored spec distinguishes an absent property from [] and
        # follows OpenRig's Array.isArray(pods) format selection.
        flat = {"name": "compiler-team", "pods": None,
                "nodes": [{"id": "worker", "runtime": "pi", "cwd": "./src"}]}
        flat_source = directory / "flat.json"
        flat_source.write_text(json.dumps(flat), encoding="utf-8")
        flat_output = directory / "flat-prepared.json"
        run(command(flat_output, input_path=flat_source, expected=None, pod=None))
        flat_expected = copy.deepcopy(flat)
        flat_expected["nodes"][0]["launch_wrapper"] = prefix
        assert json.loads(flat_output.read_bytes()) == flat_expected
        flat["nodes"][0]["launch_wrapper"] = []
        flat_source.write_text(json.dumps(flat), encoding="utf-8")
        rejected = directory / "empty-is-not-absent.json"
        run(command(rejected, input_path=flat_source, expected=None, pod=None), 2)
        assert not rejected.exists()
        run(command(directory / "empty-prepared.json", input_path=flat_source, expected=[], pod=None))

        # Existing files, directories and even dangling symlinks are never
        # replaced. Writing over the source is the same forbidden operation.
        saved_output = output.read_bytes()
        run(command(output), 1)
        assert output.read_bytes() == saved_output
        run(command(source), 1)
        existing_directory = directory / "existing-directory"
        existing_directory.mkdir()
        run(command(existing_directory), 1)
        dangling = directory / "dangling.json"
        dangling.symlink_to("missing-target.json")
        run(command(dangling), 1)
        assert dangling.is_symlink()
        assert dangling.readlink() == pathlib.Path("missing-target.json")

        outside = root / "moved.json"
        run(command(outside), 2)
        assert not outside.exists(), "Moving a spec would change its relative path context."

        # OpenRig uses dirname(path.resolve(spec)) and resolves relative cwd
        # lexically under it. Two names for the same physical directory do
        # not therefore have the same spec root when a symlink is involved.
        work = root / "work"
        team = root / "project" / "team"
        work.mkdir()
        team.mkdir(parents=True)
        alias = work / "alias"
        alias.symlink_to(team, target_is_directory=True)
        alias_document = copy.deepcopy(original)
        alias_document["pods"][0]["members"][0]["cwd"] = "../source"
        alias_bytes = json.dumps(alias_document).encode()
        alias_input = alias / "rig.json"
        alias_input.write_bytes(alias_bytes)
        changed_root_output = team / "prepared.json"
        changed_root = subprocess.run(
            command(changed_root_output, input_path=alias_input),
            cwd=root, capture_output=True, timeout=10)
        assert changed_root.returncode == 2, (
            "A directory alias cannot change the lexical spec root: ../source "
            "would move from work/source to project/source.")
        assert not changed_root_output.exists()
        assert alias_input.read_bytes() == alias_bytes

        same_alias_output = alias / "prepared.json"
        lexical_input = str(alias) + "/../alias/rig.json"
        run(command(same_alias_output, input_path=lexical_input))
        alias_expected = copy.deepcopy(alias_document)
        alias_expected["pods"][0]["members"][0]["launch_wrapper"] = prefix + previous
        assert json.loads(same_alias_output.read_bytes()) == alias_expected
        assert alias_input.read_bytes() == alias_bytes

        # Two legitimate callers racing to one destination have one publisher;
        # the loser cannot replace the winner's already complete output.
        raced = directory / "raced.json"
        first = subprocess.Popen(command(raced), cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        second = subprocess.Popen(command(raced), cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        first_output = first.communicate(timeout=10)
        second_output = second.communicate(timeout=10)
        assert sorted([first.returncode, second.returncode]) == [0, 1]
        assert first_output[0] == second_output[0] == b""
        assert json.loads(raced.read_bytes()) == expected_document
        assert source.read_bytes() == source_bytes
        assert not list(directory.glob(".openrig-member-*"))

        invalid = directory / "invalid.json"
        invalid.write_text("name: compiler-team\n", encoding="utf-8")
        invalid_output = directory / "invalid-output.json"
        run(command(invalid_output, input_path=invalid), 2)
        run(command(invalid_output, wrapper=prefix[:-1]), 2)
        run(command(invalid_output) + ["--rig", "compiler-team"], 2)
        assert not invalid_output.exists()

        help_result = subprocess.run([str(program), "--help"], capture_output=True, timeout=10)
        assert help_result.returncode == 0
        assert b"does not install a wrapper or enroll a live seat" in help_result.stdout


if __name__ == "__main__":
    main()
