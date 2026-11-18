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
| Package targets | Linux x86_64 and macOS arm64. |
| Launcher discovery | Finds `rig` on the supplied PATH and checks its OpenRig identity through root help. |
| Saved launcher recognition | Recognizes a literal `rig` invocation, including arguments and supported literal shell quoting. |
| Launch preparation | Explicitly unmanaged: preserves the original shell, arguments, environment, profile, directory and approval policy. |
| Initial model prompt | Unavailable; an explicit request is rejected without acknowledging delivery. |
| Native hooks, lifecycle and model instructions | Unavailable; no hook, coordination or model-context capability is declared. |
| Authored-member hook installation and existing-seat enrollment | Unavailable through the saved launcher. |
| Internal member library | Composes a wrapper for one explicitly selected authored member and reads public native launch identity; not connected to launch preparation. |
| Windows/WSL bridge | Unavailable; no Windows package is produced. |

Discovery checks the executable's identity. Recognition describes the authored
command's syntax; it does not claim a native runtime or enrolled seat. Neither
operation starts a dashboard, launches a model, restarts a seat or edits a rig.

The package sidecars and protocol handshake declare the implemented capability
set. The package is not included in the official bundle roster.

## Build

Use CMake 3.25 or newer and a C++20 compiler for the target platform. Linux
packaging also requires Python 3.11 or newer and `patchelf`. Use the maintained
[Linux assistant SDK and build queue](https://github.com/Varinomics/logonomic/blob/master/docs/linux_assistant_sdk.md)
for assistant Linux builds.

With a current Logonomic checkout next to this repository and the toolchain
environment activated:

```sh
queued-build --slots 1 -- cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=../logonomic/sdk/logonomic_cli_addon_sdk
queued-build --slots 4 -- cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Without the SDK source override, CMake fetches the SDK from Logonomic's `master`
branch. An embedding build supplies the SDK target and sets
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
[package contract](https://github.com/Varinomics/logonomic/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md).

## Native integration boundary

The authoritative integration requirements are in Logonomic's
[OpenRig hook support plan](https://github.com/Varinomics/logonomic/blob/master/docs/openrig_hook_support_plan.md).
Hook activation depends on independent host authority and instruction state for
each seat, reusable native-provider hook preparation, and a durable wrapper
entry point. The maintained
[Claude preparation](https://github.com/Varinomics/vnm_logonomic_cli_claude/blob/master/src/claude_preparation.cpp)
and [Codex preparation](https://github.com/Varinomics/vnm_logonomic_cli_codex/blob/master/src/codex_preparation.cpp)
own native hook behavior; OpenRig must compose those hooks with its own.

Existing seats also need a supported targeted wrapper update for the next
user-directed native launch, preserving the rig, seat and history. OpenRig's
[launch-wrapper contract](https://github.com/mvschwarz/openrig/blob/main/docs/reference/rig-spec.md#launch-wrapper-runtime-pi-and-codex)
does not supply a live edit operation. Opening an unscoped dashboard or replacing
an entire rig cannot satisfy enrollment. Windows support additionally requires
a scoped authenticated transport from WSL2 to the Windows host.

