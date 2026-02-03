# OBS Plugin Template 

## Introduction

The plugin template is meant to be used as a starting point for OBS Studio plugin development. It includes:

* Boilerplate plugin source code
* A CMake project file
* GitHub Actions workflows and repository actions

## Supported Build Environments

| Platform  | Tool   |
|-----------|--------|
| Windows   | Visual Studio 17 2022 |
| macOS     | XCode 16.0 |
| Windows, macOS  | CMake 3.30.5 |
| Ubuntu 24.04 | CMake 3.28.3 |
| Ubuntu 24.04 | `ninja-build` |
| Ubuntu 24.04 | `pkg-config`
| Ubuntu 24.04 | `build-essential` |

## Quick Start

An absolute bare-bones [Quick Start Guide](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide) is available in the wiki.

## Rules for success (follow this — AI and human)

**One check:** Before commit, run `./build-aux/check-conventions .` (CI runs it; merge fails if it fails).

**One rule:** Doc first. No new lib without adding it to `build-aux/allowed-libs.txt` and [docs/CONVENTIONS.md](docs/CONVENTIONS.md) §3. No new feature without a task in [docs/PROJECT_PLAN_TRANSCRIPTION.md](docs/PROJECT_PLAN_TRANSCRIPTION.md) or [docs/PROGRESS.md](docs/PROGRESS.md). No new naming/convention without updating CONVENTIONS. If a change would break that, say so—don’t agree blindly.

**Phase gate:** Don’t start the next phase until the current one is verified (run “How to verify” in [docs/DEFINITION_OF_DONE.md](docs/DEFINITION_OF_DONE.md) for the current phase).

**Hold the AI to it:** If you’re not sure I followed this, say: “Did you follow the Rules for success?” or “Check the rules.” I will re-read this section and confirm.

Detailed refs: [docs/README.md](docs/README.md), [CONVENTIONS](docs/CONVENTIONS.md), [STRUCTURE](docs/STRUCTURE.md), [GUARDRAILS](docs/GUARDRAILS.md), [DEFINITION_OF_DONE](docs/DEFINITION_OF_DONE.md), [PROGRESS](docs/PROGRESS.md).

---

## Focus, progress & conventions (reference)

| Doc | Purpose |
|-----|--------|
| [docs/README.md](docs/README.md) | Doc map, rules, and update checklist |
| [docs/PROJECT_PLAN_TRANSCRIPTION.md](docs/PROJECT_PLAN_TRANSCRIPTION.md) | Requirements and phases |
| [docs/PROGRESS.md](docs/PROGRESS.md) | Current phase, task status |
| [docs/CONVENTIONS.md](docs/CONVENTIONS.md) | Coding rules, naming, libs |
| [docs/STRUCTURE.md](docs/STRUCTURE.md) | NestJS-style layout |
| [docs/GUARDRAILS.md](docs/GUARDRAILS.md) | What we guard against |
| [docs/DEFINITION_OF_DONE.md](docs/DEFINITION_OF_DONE.md) | How to verify each phase |
| [docs/ASSUMPTIONS.md](docs/ASSUMPTIONS.md) | What we’re assuming |
| [docs/DOCUMENTATION.md](docs/DOCUMENTATION.md) | OBS & Whisper doc links; what to read when (Phase 1/2); local refs in docs/reference/ |
| [docs/BUILD_VERIFY.md](docs/BUILD_VERIFY.md) | Build + load verification (presets, install, Phase 1 manual verify) |

Before commit: `./build-aux/check-conventions .`. New libs: add to [build-aux/allowed-libs.txt](build-aux/allowed-libs.txt) and CONVENTIONS §3. Before next phase: verify current phase (DEFINITION_OF_DONE).

## Documentation

All documentation can be found in the [Plugin Template Wiki](https://github.com/obsproject/obs-plugintemplate/wiki).

Suggested reading to get up and running:

* [Getting started](https://github.com/obsproject/obs-plugintemplate/wiki/Getting-Started)
* [Build system requirements](https://github.com/obsproject/obs-plugintemplate/wiki/Build-System-Requirements)
* [Build system options](https://github.com/obsproject/obs-plugintemplate/wiki/CMake-Build-System-Options)

## GitHub Actions & CI

Default GitHub Actions workflows are available for the following repository actions:

* `push`: Run for commits or tags pushed to `master` or `main` branches.
* `pr-pull`: Run when a Pull Request has been pushed or synchronized.
* `dispatch`: Run when triggered by the workflow dispatch in GitHub's user interface.
* `build-project`: Builds the actual project and is triggered by other workflows.
* `check-format`: Checks CMake and plugin source code formatting and is triggered by other workflows.

The workflows make use of GitHub repository actions (contained in `.github/actions`) and build scripts (contained in `.github/scripts`) which are not needed for local development, but might need to be adjusted if additional/different steps are required to build the plugin.

### Retrieving build artifacts

Successful builds on GitHub Actions will produce build artifacts that can be downloaded for testing. These artifacts are commonly simple archives and will not contain package installers or installation programs.

### Building a Release

To create a release, an appropriately named tag needs to be pushed to the `main`/`master` branch using semantic versioning (e.g., `12.3.4`, `23.4.5-beta2`). A draft release will be created on the associated repository with generated installer packages or installation programs attached as release artifacts.

## Signing and Notarizing on macOS

Basic concepts of codesigning and notarization on macOS are explained in the correspodning [Wiki article](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS) which has a specific section for the [GitHub Actions setup](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS#setting-up-code-signing-for-github-actions).
