# DockStation installer safety checks

This document describes the installer hardening integrated for
`chore/flatten-repka-directories`.

## Install order

The installer validates its arguments first. `--bundle` requires a non-empty
archive path; an empty value is an error and never falls back to source-build
mode.

For bundle installation, it copies the supplied archive into a private
`mktemp` directory. The external `.sha256` sidecar must contain exactly one
standard `sha256sum` line, and the recorded filename must exactly match the
archive basename. The digest is checked against the private snapshot. Both the
archive verifier and extraction step then use that same snapshot, so a change
to the caller-provided source file after verification cannot change the
payload that is installed.

Before building or copying binaries, unit files, or configuration, the
installer checks each destination directory and file. Existing symlinks,
non-directory path components, and non-regular file destinations are rejected.
The checks cover the staging root, `/usr`/`/usr/local` install hierarchy, and
`/etc/dockstation` configuration path.

For a real installation (not `DOCKSTATION_INSTALL_ROOT` staging), the
read-only `check_service_identity.sh` checks any existing `dockstation` group
and user before files are installed. It rejects GID/UID 0, a primary-GID
mismatch, unexpected home directory, interactive shell, ambiguous NSS records,
and account lookup errors. If neither account exists, the existing installer
path creates them as a system group and a non-login system user. Staging mode
continues to avoid changing or depending on host accounts and systemd state.

## Bundle verifier

The verifier accepts only the allow-listed DockStation bundle paths and
rejects duplicate archive members. Every archive entry must be an ordinary
file or directory of the expected type; symlinks, hardlinks, FIFOs, sockets,
block/character devices, and unknown entry types are rejected before
extraction.

The internal `SHA256SUMS` must cover the exact seven expected payload files,
exactly once. Its paths are validated before `sha256sum --check` is allowed to
read them, preventing a crafted manifest from directing the verifier to hash
files outside the extracted bundle. The existing AArch64 ELF, optional ABI
ceiling, and mandatory native `ldd` checks remain in place.

## ARM64 release bundle audit

The build job writes a versioned runtime dependency report containing one
sorted SONAME list for each executable. It deliberately omits ASLR-dependent
memory addresses and absolute library paths. During bundle creation, CI
compares this report exactly with the build runner's `ldd` output. On the
target, the verifier requires each recorded SONAME to resolve; additional
target-specific transitive dependencies are allowed.

The bundle manifest records the exact checked-out Git commit, commit-derived
source-date epoch, build OS/kernel, compiler, CMake, OpenSSL 3 build version,
Release-only profile, disabled SQLite persistence, install prefix, and ABI
ceilings. CI checks the recorded source commit against the actual checkout.

Tar entries are sorted by path and normalized to numeric root ownership and the
source commit timestamp. Gzip timestamps and original filenames are omitted.
CI packages the same payload twice and requires byte-identical archives. This
tests deterministic archive construction from one staged install tree; it does
not claim independent bit-for-bit reproducibility of two compiler builds.

## Cryptographic artifact provenance

The ARM64 release workflow now produces a GitHub Artifact Attestation for the
exact `dockstation-repka-pi5-arm64.tar.gz` archive. GitHub Actions requests a
short-lived Sigstore signing certificate using its OIDC identity and publishes
a signed SLSA build-provenance attestation. The final 30-day Actions artifact
contains the archive, its SHA-256 sidecar, and the detached
`dockstation-repka-pi5-arm64.tar.gz.attestation.jsonl` bundle.

Signing is isolated in a separate job with only the permissions needed to
download the build artifact and write/read attestations. It runs only for
pushes or manual runs on `chore/flatten-repka-directories`; pull-request code
does not receive the signing job. The job verifies the provenance through the
GitHub API and then verifies the downloaded JSONL bundle. Negative controls
require rejection of a modified archive, an unexpected signer workflow, and
an unexpected source commit. The verifier also rejects self-hosted-runner
attestations.

For online verification, use a current GitHub CLI:

```sh
gh attestation verify dockstation-repka-pi5-arm64.tar.gz \
  --repo GooseRabbit/dockstation \
  --signer-workflow GooseRabbit/dockstation/.github/workflows/linux-cmake.yml \
  --source-ref refs/heads/chore/flatten-repka-directories \
  --source-digest <40-character-source-commit>
```

The source commit is recorded in `BUNDLE-MANIFEST.txt` and printed in the
workflow summary. For offline verification, the JSONL sidecar is distributed
with the archive; first obtain a Sigstore trusted-root file online with
`gh attestation trusted-root > trusted_root.jsonl`, then run
`gh attestation verify` with `--bundle <sidecar>` and
`--custom-trusted-root trusted_root.jsonl`. Because offline verification
cannot query the current repository identity, compare the verified certificate's
`sourceRepositoryIdentifier` with the numeric repository ID obtained online
using `gh api repos/GooseRabbit/dockstation --jq .id`.

SHA-256 sidecars remain useful for detecting accidental corruption, but a
checksum alone is **not a digital signature**: an attacker able to replace both
the archive and its sidecar can generate a new matching pair. The attestation
binds the archive digest to the expected GitHub repository/workflow identity
and source revision. It does not prove that the source code is safe, that the
workflow itself was uncompromised, or that the binary is compatible with the
physical Repka OS image. It is a signed provenance check, not an independent
reproducible-build guarantee.

The installer deliberately does not invoke GitHub CLI or validate the
attestation automatically. Operators must verify provenance on a trusted
management computer before copying the bundle to Repka; installer success by
itself does not establish artifact origin.

## Recorded validation result

GitHub Actions run [43746375584](https://github.com/GooseRabbit/dockstation/actions/runs/43746375584)
completed successfully on both build architectures and the isolated signer job.
Each architecture passed 23/23 CTest cases; x64 passed all 31 host test programs
and 141 Python tests. Operator/QML/e2e, shell syntax and the full Repka shell-test
loop also passed. The native ARM64 Release bundle passed verifier and two staging
installations without CMake.

The signed distribution artifact is ID `14174691384` (30-day retention). Its archive
SHA-256 is `11c5024b765bc3f8311ca031d46288038bbfc269fd7235ccd6fce91cf68550a5`.
GitHub attestation ID `67847564` was published to Rekor log index `3907782864`.
CI verified the repository attestation and the detached JSONL bundle, then confirmed
that altered archive bytes, an unexpected signer workflow, and a different source
commit are rejected. The final archive, SHA sidecar and detached attestation JSONL
were uploaded together.

## Automated tests

- `verification/repka_gateway/test_install_gateway_hardening.sh` covers empty
  bundle arguments, source and bundle staging, configuration preservation on
  repeat installation, strict sidecar binding, multi-line sidecars, symlinked
  destinations and parent directories, source-archive mutation after
  verification, duplicate archive members, incomplete checksum manifests, and
  special archive file types.
- `verification/repka_gateway/test_service_identity_preflight.sh` covers
  missing accounts, valid existing accounts, root UID/GID, invalid IDs,
  mismatched primary group, interactive shell, unexpected home, ambiguous NSS
  records, and lookup errors.

These are shell-level tests using synthetic bundles and command stubs. They do
not perform a real target installation, change host accounts, invoke the
host's systemd manager, or verify physical hardware.
