# Xenon updates

Windows x64 builds use GitHub Releases for discovery and Telegram's existing
signed v2 packages, downloader, staging directory and Windows installer for
installation. The client never requests Telegram's update feeds on this platform.

Xenon Settings contains **Automatically check for updates** (enabled by default),
**Automatically install updates** (disabled by default) and **Check now**.
Automatic checks run at startup and hourly. Automatic installation downloads and
prepares a package in the background, then installs it on the next restart.
With automatic installation disabled, **Update** downloads the package and
restarts Xenon after verification. **Skip** remembers only that release and
discards a prepared update. Checking manually includes skipped releases.

The update dialog lists every commit between the installed and candidate hashes,
including intermediate releases. GitHub compare results are paginated; unrelated
or diverged histories are rejected. Network failures and incomplete metadata are
reported as errors rather than as an up-to-date installation.

## Build and release identity

Configuration embeds the current Git commit hash and its committer timestamp.
The Windows workflow also passes `XENON_BUILD_NUMBER` from `github.run_number`.
The signed package version combines the Telegram base version and this build
number, so rebuilding the same Telegram version can still produce an update.
Keep that sequence increasing within a Telegram base version when changing the
workflow. Dates are displayed for context; version ordering and Git ancestry
prevent accidental downgrades or updates from another branch.

The build artifact and release include `Telegram.exe`, `Updater.exe`, a signed
`td-update-win-x64-*` package and `xenon-update.json`. The latter records the
source commit, time, base version, build number and package name. Release notes
include commits since the preceding release. A delayed older build cannot replace
a newer release as latest.

## Signing keys

Public trust files live under `Telegram/Resources/xenon-update`. The root key
signs the manifest, which authorizes a separate Ed25519 release signing key.
The client verifies the package before decompression and checks its signed
version against the release it selected.

GitHub Actions requires the repository secret `XENON_UPDATE_SIGNING_KEY`, holding
the release private PEM. The root private key stays offline. Neither private key
belongs in Git or uploaded build artifacts. To bootstrap another installation,
use Python with `cryptography` and fresh directories:

```powershell
python .github/scripts/xenon_updates.py init-keys --public-dir <public-directory> --private-dir <private-directory>
Get-Content -Raw <private-directory>/release-private.pem | gh secret set XENON_UPDATE_SIGNING_KEY --repo miuichina/xenondesktop
```

Replace public trust files only as part of a planned key migration: installed
clients retain their embedded root. Back up private keys outside the build tree.
The first build containing this updater must be installed manually because older
Xenon builds have their updater compiled out.
