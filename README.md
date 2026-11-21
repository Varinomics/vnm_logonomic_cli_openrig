# Logonomic OpenRig add-on

This package discovers and recognizes [OpenRig](https://github.com/mvschwarz/openrig)
launchers in Logonomic. OpenRig is distributed as `@openrig/cli` and runs through
the `rig` command. Install OpenRig separately.

Saved launchers run unchanged as ordinary terminals. The add-on does not install
native hooks or enroll seats. A dashboard represents independent native seats;
it is not a model conversation to which Logonomic can assign shared authority.

## Support matrix

| Capability | Support |
| --- | --- |
| Package targets | Windows x86_64, Linux x86_64 and macOS arm64. |
| Launcher discovery | Finds `rig` on the supplied PATH and checks its OpenRig identity through root help. |
| Saved launcher recognition | Recognizes a literal `rig` invocation, including arguments and supported literal shell quoting. |
| Launch preparation | Explicitly unmanaged: preserves the original shell, arguments, environment, profile, directory and approval policy. |
| Initial model prompt | Unavailable; an explicit request is rejected without acknowledging delivery. |
| Native hooks, lifecycle and model instructions | Unavailable; no hook, coordination or model-context capability is declared. |
| Authored-member hook installation and existing-seat enrollment | Unavailable through the saved launcher. |
| Authored-spec preparation on Linux and macOS | An explicit command composes a caller-supplied wrapper for one selected member into a new JSON spec. It does not install the wrapper or enroll a seat. |
| Internal native identity reader | Reads public native launch identity for future authenticated binding; not connected to launch preparation. |
| Windows and WSL2 | Discovers a guest `rig` executable in running WSL2 distributions and preserves explicit saved WSL launches as ordinary terminals. Guest seat observation requires a separate authenticated transport. |

Discovery checks the executable's identity. Recognition describes the authored
command's syntax; it does not claim a native runtime or enrolled seat. Neither
operation starts a dashboard, launches a model, restarts a seat or edits a rig.

On Windows, install OpenRig and its prerequisites in the Linux distribution
where you use it. Discovery verifies the guest executable's root help and saves
the system `wsl.exe` with the distribution and absolute guest path. A saved
Direct launcher can also select a registered WSL2 distribution explicitly:

```text
wsl.exe --distribution Ubuntu-24.04 --exec /usr/local/bin/rig
```

Use your distribution name and its actual executable path. Optional
`--user` and `--cd` select the guest user and working directory before
`--exec`. Arguments after the executable go to OpenRig. Discovery leaves
stopped distributions idle; a saved launcher can start its selected distribution.

The package sidecars and protocol handshake declare the implemented capability
set. The package is not included in the official bundle roster.

## Build

The [contributor guide](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/CONTRIBUTING.md) covers local package checks.
Send provider pull requests to [the openrig repository](https://github.com/Varinomics/vnm_logonomic_cli_openrig/pulls).

Use CMake 3.25 or newer and a C++20 compiler for the target platform. Linux
packaging also requires Python 3.11 or newer, `patchelf` and binutils.
CMake fetches the SDK from `vnm_logonomic_cli_addon` on `master`. For a local
checkout, pass `-DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/path/to/sdk/logonomic_cli_addon_sdk`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

An embedding build supplies the SDK target and sets
`LOGONOMIC_CLI_ADDON_EMBEDDED`; the same package recipe then registers its output
with that build without configuring the standalone tests.

## Use a development package

A single-configuration build writes the indexed package to
`build/package/logonomic.cli.openrig`. Set the profile's add-on development root
to the absolute path of `build/package`:

```ini
[cli_addons]
development_root=/absolute/path/to/vnm_logonomic_cli_openrig/build/package
```

Start Logonomic with that profile to load the package generation. Save a launcher
such as `rig` or an explicitly selected `rig attach` command with the arguments
you use. The saved command remains unchanged. For multi-configuration builds,
the root is `build/package/<configuration>`.

`cmake --install build --prefix <fresh-output-directory>` exports the indexed
package to a new destination. Package layout, integrity and application-managed
installation follow the SDK's
[package contract](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md).

## Prepare an authored member

The Linux and macOS packages include `helpers/openrig_prepare_member` for an explicitly selected
member in a JSON-authored spec. JSON is accepted by OpenRig's YAML spec reader.
The command creates a new file beside the source, preserving the directory used
by relative paths. It leaves the source bytes unchanged and refuses to replace
any existing output path.

```sh
build/package/logonomic.cli.openrig/helpers/openrig_prepare_member \
    --spec /work/team/rig.json --output /work/team/rig-prepared.json \
    --rig compiler-team --pod build --member worker \
    --expect-wrapper 'null' \
    --wrapper-prefix '["/path/to/your/stable-wrapper","installation-reference","--"]'
```

Supply the exact existing `launch_wrapper` array to `--expect-wrapper`, or
`null` when the property is absent. An explicit empty array is distinct from
absence. A mismatch refuses preparation. Omit `--pod` only for the flat
`nodes[]` format.

The caller supplies an independently stable wrapper executable and nonsecret
arguments. This package does not yet supply that launch entrypoint. Do not use
a path into a temporary add-on package generation or put credentials in the
prefix. The caller-owned prefix must end with one `--` argument and contain no
earlier `--` argument after its executable; that boundary makes reapplication
unambiguous. Existing wrapper arguments remain the exact suffix, including any
of their own delimiters. A changed prefix using the same executable is refused.

The output changes only the selected member's `launch_wrapper` value. Other
JSON values are preserved, including account/profile settings, policy,
relative paths and other members; formatting may change. Reapplying an
identical prefix copies the input bytes unchanged. The command does not start
OpenRig, validate the entire upstream schema, change a live rig, obtain host
authority or activate hooks. Validate the resulting spec with the OpenRig
version you use before launching it as a new rig.

## Native integration boundary

The authoritative integration requirements are in Logonomic's
[OpenRig hook support plan](https://github.com/Varinomics/logonomic/blob/master/docs/openrig_hook_support_plan.md).
Hook activation depends on independent host authority and instruction state for
each seat, reusable native-provider hook preparation, and a durable wrapper
entry point. The maintained
[Claude preparation](https://github.com/Varinomics/vnm_logonomic_cli_claude/blob/master/src/claude_preparation.cpp)
and [Codex preparation](https://github.com/Varinomics/vnm_logonomic_cli_codex/blob/master/src/codex_preparation.cpp)
own native hook behavior; OpenRig must compose those hooks with its own.

Local Claude integration also requires a supported upstream grant-and-claim
operation for each native launch, restricted to the enrolled member and launch
admission. It must authenticate the actual launching process while a launch or
successor is still pending. OpenRig's
[launch identity metadata](https://github.com/mvschwarz/openrig/blob/da697972c54a46c702b15bc5d0835efbc5725e18/packages/daemon/src/domain/seat-launch-environment.ts)
is public, and its daemon-wide activity token cannot authorize Logonomic
enrollment. A
[successor starts before its session and binding commit](https://github.com/mvschwarz/openrig/blob/da697972c54a46c702b15bc5d0835efbc5725e18/packages/daemon/src/domain/successor-session-launcher.ts),
so committed inventory cannot authenticate that pending launch. Each accepted
launch needs fresh host authority and activation even when its OpenRig occupant
generation continues. The inspected
[run-bound activity-grant issuer](https://github.com/mvschwarz/openrig/blob/da697972c54a46c702b15bc5d0835efbc5725e18/packages/daemon/src/domain/runner-grant-launch.ts)
does not supply this local-Claude enrollment contract. Hook support remains
unavailable without it.

A stable bootstrap must survive owner exit and package-custody cleanup, and
allow the original native child to launch without hooks when no authorized
owner is available. Saved specs, global tmux environment and durable wrapper
arguments must contain no host session secrets. Native preparation must run at
the innermost wrapper, after existing wrappers have transformed the actual child's
arguments and environment, then execute the prepared child with `exec`. The
authored-spec helper above only prepends a caller-owned prefix; it does not
establish that placement or activate native hooks.

Existing seats also need a supported targeted wrapper update for the next
user-directed native launch, preserving the rig, seat and history. OpenRig's
[launch-wrapper contract](https://github.com/mvschwarz/openrig/blob/da697972c54a46c702b15bc5d0835efbc5725e18/docs/reference/rig-spec.md)
does not supply a live edit operation. Opening an unscoped dashboard or replacing
an entire rig cannot satisfy enrollment. Windows seat observation additionally requires
a scoped authenticated transport from WSL2 to the Windows host.

The linked OpenRig revision records the inspected source. Compatibility depends
on the required capabilities, not an exact revision.
