# Work Order: Add Manual Unsigned DLL-Only Release CI

Date: 2026-09-08  
Repository: `onehoon/OptiScaler`  
Target branch for this work order: `master`  
Primary implementation area: `.github/workflows/`  
Expected implementation size: one new workflow file, with no production C++ changes

---

# 1. Goal

Add a new, isolated GitHub Actions workflow that can manually build an arbitrary OptiScaler branch/tag/commit and publish only the compiled OptiScaler DLL in two minimal ZIP packages.

The required release assets are exactly:

```text
OptiScaler.zip
└── OptiScaler.dll


dxgi.zip
└── dxgi.dll
```

`dxgi.dll` must be a byte-identical copy of the same newly built `OptiScaler.dll`, renamed only at packaging time.

The workflow is intentionally **unsigned**.

Do not attempt to use the existing SignPath integration, and do not require any signing secret.

The workflow must be manual-only. It must not run on push, pull request, schedule, or tag creation.

---

# 2. Product Requirements

The new CI must satisfy all of the following.

## 2.1 Build target selection

The user must be able to choose which repository revision is built.

Support at least:

- branch names;
- tags;
- full or short commit SHA values that `actions/checkout` can resolve.

Use a `workflow_dispatch` string input such as:

```yaml
inputs:
  target_ref:
    description: "Branch, tag, or commit SHA to build"
    required: true
    default: "master"
```

Do **not** rely only on GitHub Actions' native "Use workflow from" / branch dropdown.

Reason: the new workflow will initially exist on `master`, while older or active feature branches may not contain the new workflow file themselves. A dedicated `target_ref` input allows the workflow definition to stay on `master` while `actions/checkout` explicitly checks out the requested source revision.

The workflow should therefore be launched from the default branch and then build:

```yaml
ref: ${{ inputs.target_ref }}
```

This is the authoritative source revision for the build.

## 2.2 Build configuration

Build the current OptiScaler solution/project using the existing repository build configuration:

```text
Configuration: Release
Platform: x64
Solution: OptiScaler.sln
```

The repository currently contains one solution project:

```text
OptiScaler\OptiScaler.vcxproj
```

Use the repository's existing MSBuild-based build path rather than introducing CMake, Ninja, or a second build system.

A suitable command is expected to be equivalent to:

```powershell
msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
```

Explicitly passing `Platform=x64` is preferred for this workflow even though existing workflows often rely on the current solution defaults.

## 2.3 Submodules

Checkout must include submodules because OptiScaler depends on SDK/header/library content from repository submodules.

Use:

```yaml
submodules: recursive
```

or another equivalent setting that fully initializes the required nested submodules.

Do not remove submodule checkout merely because the final release package contains one DLL.

## 2.4 Authoritative DLL output

The current Release x64 project PostBuild event moves the built DLL into:

```text
x64\Release\a\OptiScaler.dll
```

The new workflow may allow the existing PostBuild steps to continue creating/copying other files under `x64\Release\a\`, but it must ignore those files when packaging.

The only build product consumed by this CI is:

```text
${{ github.workspace }}\x64\Release\a\OptiScaler.dll
```

Add an explicit validation step after MSBuild:

```powershell
$dll = Join-Path $env:GITHUB_WORKSPACE "x64\Release\a\OptiScaler.dll"
if (-not (Test-Path $dll)) {
    throw "Expected build output not found: $dll"
}
```

Do not silently fall back to another DLL from another directory if this file is missing. A missing expected output must fail the workflow.

---

# 3. Packaging Requirements

Create a clean staging area that is independent of the existing `x64\Release\a\` package tree.

For example:

```text
_artifacts\
  OptiScaler\
    OptiScaler.dll
  dxgi\
    dxgi.dll
```

Start from an empty staging directory on every run.

## 3.1 OptiScaler.zip

Copy only:

```text
x64\Release\a\OptiScaler.dll
```

into the OptiScaler staging directory and create:

```text
OptiScaler.zip
```

The ZIP root must contain exactly:

```text
OptiScaler.dll
```

There must not be an extra parent folder inside the ZIP.

Do not include:

- `OptiScaler.ini`;
- XeSS DLLs;
- FidelityFX DLLs;
- Agility SDK DLLs;
- licenses;
- setup scripts;
- PDB files;
- `.lib` files;
- `.exp` files;
- `resource_build_*` files;
- any other file copied by the normal PostBuild packaging process.

## 3.2 dxgi.zip

Take the exact same `OptiScaler.dll` from the same build and copy it as:

```text
dxgi.dll
```

Create:

```text
dxgi.zip
```

The ZIP root must contain exactly:

```text
dxgi.dll
```

Do not compile a second binary for `dxgi.dll`.

Do not change compiler/linker flags for the dxgi package.

Do not patch PE metadata.

The operation is only:

```text
OptiScaler.dll bytes -> copy -> filename dxgi.dll
```

## 3.3 Byte identity validation

Before creating/uploading the release assets, verify that the staged `OptiScaler.dll` and `dxgi.dll` are byte-identical.

A recommended PowerShell check is:

```powershell
$optiHash = (Get-FileHash $optiDll -Algorithm SHA256).Hash
$dxgiHash = (Get-FileHash $dxgiDll -Algorithm SHA256).Hash

if ($optiHash -ne $dxgiHash) {
    throw "dxgi.dll is not byte-identical to OptiScaler.dll"
}
```

Log the SHA-256 hash so a build can be identified later.

---

# 4. ZIP Creation

Use standard ZIP archives, not 7z archives renamed to `.zip`.

PowerShell `Compress-Archive` is acceptable.

Example shape:

```powershell
Compress-Archive \
    -Path "$stageOpti\OptiScaler.dll" \
    -DestinationPath "$artifactRoot\OptiScaler.zip" \
    -Force

Compress-Archive \
    -Path "$stageDxgi\dxgi.dll" \
    -DestinationPath "$artifactRoot\dxgi.zip" \
    -Force
```

After ZIP creation, validate the archive contents rather than assuming the command produced the intended structure.

The workflow should fail if either ZIP contains anything other than its single expected DLL.

If using PowerShell/.NET ZIP inspection, the acceptance conditions are:

```text
OptiScaler.zip entries == ["OptiScaler.dll"]
dxgi.zip entries       == ["dxgi.dll"]
```

---

# 5. Signing Policy

This workflow is **unsigned-only** by design.

The repository currently has a signing workflow that uses SignPath and repository secrets. Do not reuse it here.

Specifically, the new workflow must not reference:

```text
SIGNPATH_API_TOKEN
signpath/github-action-submit-signing-request
release-signing
signed\OptiScaler.dll
```

No certificate import, Azure signing, self-signed certificate, test signing, or alternative code-signing mechanism is in scope.

The produced DLL should be the direct unsigned result of the normal Release build.

The GitHub Release metadata should clearly say that the packages are unsigned CI builds.

---

# 6. GitHub Release Policy

Use a single fixed prerelease as the publication target.

Recommended tag:

```text
unsigned-ci
```

Recommended release name:

```text
OptiScaler Unsigned CI Build
```

Recommended properties:

```text
draft: false
prerelease: true
```

The purpose is to provide stable asset names/URLs while replacing the package contents whenever the workflow is manually run for another ref.

## 6.1 First run

If the release/tag does not exist, create it.

## 6.2 Subsequent runs

If the release already exists:

- keep the same release/tag;
- replace `OptiScaler.zip`;
- replace `dxgi.zip`;
- update the release body with metadata for the newly built ref.

Use release asset replacement semantics such as:

```bash
gh release upload unsigned-ci OptiScaler.zip dxgi.zip --clobber
```

or an equivalent well-maintained GitHub Action that deterministically replaces assets of the same name.

Do not create timestamped duplicate assets.

Do not retain previous `OptiScaler.zip` / `dxgi.zip` assets alongside the current ones.

The final fixed release must contain only the two intended package assets owned by this workflow, unless the repository owner later explicitly expands the scope.

## 6.3 Release metadata

The release body should be rewritten each run to state at least:

```text
Unsigned CI build
Requested ref: <workflow input>
Resolved commit: <full SHA>
Resolved short commit: <short SHA>
Build configuration: Release x64
SHA-256: <hash of DLL>
Build date/time: <UTC timestamp>
```

Optionally include the resolved branch/tag information when Git can determine it reliably.

Do not present this as an official upstream signed release.

---

# 7. Permissions

Use the minimum GitHub Actions permission needed for release publication:

```yaml
permissions:
  contents: write
```

Do not add broad permissions such as:

```text
actions: write
pull-requests: write
issues: write
packages: write
id-token: write
```

unless a concrete implementation requirement appears and is documented.

The standard repository-scoped `GITHUB_TOKEN` should be sufficient to create/edit the release and upload assets.

No new repository secret should be required.

---

# 8. Concurrency

Because this workflow updates one fixed release and fixed asset names, concurrent runs can overwrite one another in an undefined order.

Prevent simultaneous publication using a workflow-level concurrency group.

Recommended:

```yaml
concurrency:
  group: optiscaler-unsigned-dll-release
  cancel-in-progress: false
```

Do not use `cancel-in-progress: true` unless there is a strong reason. A manually requested build should normally finish rather than being cancelled merely because another build was started.

Serialization is sufficient.

---

# 9. Suggested Workflow Structure

Create one new file, for example:

```text
.github/workflows/build-dll-release-unsigned.yml
```

Do not modify these existing workflows unless absolutely required:

```text
.github/workflows/build.yml
.github/workflows/just_build.yml
.github/workflows/just_build_no_signature.yml
.github/workflows/release_debug.yml
```

Recommended logical structure:

```yaml
name: Build DLL Release (Unsigned)

on:
  workflow_dispatch:
    inputs:
      target_ref:
        description: "Branch, tag, or commit SHA to build"
        required: true
        default: "master"

env:
  BUILD_CONFIGURATION: Release

permissions:
  contents: write

concurrency:
  group: optiscaler-unsigned-dll-release
  cancel-in-progress: false

jobs:
  build-and-release:
    runs-on: windows-latest

    steps:
      - name: Checkout selected ref
        uses: actions/checkout@v6
        with:
          ref: ${{ inputs.target_ref }}
          fetch-depth: 0
          submodules: recursive

      - name: Add MSBuild to PATH
        uses: microsoft/setup-msbuild@v2

      - name: Build Release x64
        shell: powershell
        run: |
          msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal

      - name: Validate and package DLLs
        shell: powershell
        run: |
          # Resolve x64\Release\a\OptiScaler.dll
          # Create clean staging directories
          # Copy OptiScaler.dll
          # Copy same bytes as dxgi.dll
          # Verify SHA-256 identity
          # Create OptiScaler.zip
          # Create dxgi.zip
          # Inspect each ZIP and require exactly one expected entry
          # Export resolved SHA/hash/timestamp for release notes

      - name: Create or update unsigned-ci release
        shell: powershell
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          # Create prerelease if missing
          # Otherwise update release notes
          # Upload OptiScaler.zip and dxgi.zip with replacement/clobber semantics
```

This is guidance, not a requirement to reproduce the snippet verbatim. Prefer clear and robust implementation over unnecessary abstraction.

---

# 10. Important Checkout / Ref Handling Details

The workflow must report the **actual checked-out commit**, not `github.sha` from the workflow definition branch.

After checkout, derive the build commit from Git:

```powershell
$resolvedSha = (git rev-parse HEAD).Trim()
$shortSha = (git rev-parse --short HEAD).Trim()
```

This distinction matters because the workflow file may execute from `master` while `target_ref` points to a feature branch.

Do not use only:

```text
${{ github.sha }}
```

for release metadata unless it has been verified to represent the checked-out target ref in this workflow topology.

Also log:

```text
Requested target_ref
Resolved HEAD SHA
```

at the beginning of the build so CI output is unambiguous.

---

# 11. Ref Validation and Failure Behavior

If `target_ref` cannot be resolved by checkout, the workflow must fail.

Do not silently fall back to `master`.

If the ref resolves but the source revision does not compile, the workflow must fail and must not replace the existing release assets.

This means release publication must happen only after all of the following succeed:

```text
checkout
→ submodules
→ MSBuild
→ expected DLL existence
→ byte identity check
→ ZIP creation
→ ZIP contents validation
```

A failed build must leave the previously published `unsigned-ci` assets untouched.

---

# 12. Scope Boundaries

This task is CI/package automation only.

Do not change:

- C++ runtime behavior;
- `OptiScaler.vcxproj` build logic unless the workflow cannot work without it;
- output DLL exports;
- proxy logic;
- resource versioning;
- signing configuration used by upstream/existing workflows;
- normal full-package release contents;
- nightly release policy;
- README installation documentation.

Do not remove the existing PostBuild packaging behavior merely because this workflow only needs the DLL. That behavior is shared by other build/release paths.

Do not replace or delete the existing unsigned build workflow.

The new workflow is an additional specialized CI path.

---

# 13. Validation Requirements

Before opening the implementation PR, validate the following.

## 13.1 Static workflow validation

Check that the YAML is syntactically valid and that all GitHub Actions expressions are correctly formed.

Confirm:

```text
workflow_dispatch only
contents: write only
windows-latest
Release x64
submodules enabled
no SignPath references
no signing secrets
```

## 13.2 Manual master build

Run the new workflow with:

```text
target_ref = master
```

Expected:

```text
Build succeeds
OptiScaler.zip uploaded
 dxgi.zip uploaded
release is prerelease
release tag is unsigned-ci
```

Verify archive contents manually or in CI:

```text
OptiScaler.zip -> exactly OptiScaler.dll
 dxgi.zip      -> exactly dxgi.dll
```

## 13.3 Feature branch build

Run the same workflow from the default workflow definition with a real feature branch, for example any currently active fork branch.

Do not require the feature branch itself to already contain the new workflow file.

Confirm the release notes show:

```text
Requested ref = feature branch name
Resolved SHA = feature branch HEAD actually built
```

Confirm the assets are replaced rather than duplicated.

## 13.4 Hash validation

Extract both ZIPs and verify:

```powershell
(Get-FileHash OptiScaler.dll -Algorithm SHA256).Hash -eq \
(Get-FileHash dxgi.dll -Algorithm SHA256).Hash
```

Result must be `True`.

## 13.5 Negative validation

Test or reason through at least these failure paths:

```text
invalid target_ref
build failure
missing x64\Release\a\OptiScaler.dll
ZIP validation failure
release upload failure
```

For all pre-publication failures, the previous valid release assets must not be intentionally deleted first.

Avoid a sequence that deletes the old assets and only later attempts upload. Prefer atomic-ish `--clobber` replacement after the new ZIPs are fully ready.

---

# 14. Acceptance Criteria

The implementation is complete only when all of the following are true:

- [ ] One new manual-only unsigned DLL release workflow exists.
- [ ] The workflow accepts `target_ref` and checks out that requested branch/tag/SHA.
- [ ] Checkout includes required submodules.
- [ ] It builds `OptiScaler.sln` as `Release|x64`.
- [ ] It uses `x64\Release\a\OptiScaler.dll` as the authoritative output.
- [ ] `OptiScaler.zip` contains exactly one root entry: `OptiScaler.dll`.
- [ ] `dxgi.zip` contains exactly one root entry: `dxgi.dll`.
- [ ] The two DLL files are byte-identical.
- [ ] No INI, SDK DLL, license, setup script, PDB, or other package file is included.
- [ ] No SignPath or other signing service is invoked.
- [ ] No signing secret is required.
- [ ] The workflow creates/updates one fixed prerelease tagged `unsigned-ci`.
- [ ] Re-running replaces the two fixed assets instead of accumulating timestamped copies.
- [ ] Release notes identify the requested ref and the actual resolved commit SHA.
- [ ] A failed build does not deliberately erase the previous valid release assets.
- [ ] Concurrent runs are serialized for the fixed release target.
- [ ] Existing workflows and production C++ code are unchanged unless a concrete blocker is found and documented.

---

# 15. Deliverable / PR Expectations

Prefer one small PR.

Expected changed files:

```text
.github/workflows/build-dll-release-unsigned.yml
```

No other code file should normally be required.

PR title suggestion:

```text
Add manual unsigned DLL-only release workflow
```

PR description should state:

- manual-only workflow;
- arbitrary `target_ref` checkout;
- Release x64 unsigned build;
- two minimal ZIP assets only;
- fixed `unsigned-ci` prerelease;
- no SignPath dependency;
- validation performed on `master` and at least one feature branch if GitHub Actions execution is available.

If repository policy requires the work-order document itself to remain in the PR history, leave this document unchanged unless implementation discoveries require a factual correction.

---

# 16. Non-Goals

The following are explicitly not part of this task:

```text
signed binaries
full OptiScaler distribution archive
nightly build replacement
scheduled builds
automatic push builds
automatic PR builds
source archive publication
versioned historical build retention
multiple architecture builds
Win32/x86 builds
ReleaseDebug packages
PDB publication
SDK redistribution
installer generation
```

Keep the implementation narrowly focused on the requested CI use case.
