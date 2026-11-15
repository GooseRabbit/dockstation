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

## Integrity limitation

SHA-256 sidecars detect accidental corruption and bind the checked digest to
the requested filename. A checksum file is **not a digital signature**: an
attacker able to replace both the archive and its sidecar can generate a new
matching pair. Artifact authenticity still depends on retrieving the bundle
from the trusted CI/GitHub artifact source. A future signed-release mechanism
would be a separate security improvement.

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
