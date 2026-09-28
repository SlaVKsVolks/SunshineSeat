# SunshineSeat

SunshineSeat is a custom fork of [LizardByte Sunshine](https://github.com/LizardByte/Sunshine), a self-hosted game-streaming host for Moonlight. This repository contains a curated source snapshot with password-based new-device pairing and other local SunshineSeat host work. It is not an official Sunshine release.

## Current status

- Source snapshot: published, with a fresh Git history and the upstream GPL-3.0 license and public dependency pins.
- Windows host candidate: [password-pairing prerelease overlay](https://github.com/SlaVKsVolks/SunshineSeat/releases/tag/v2026.928.1-password-pairing-candidate). It is unsigned, replaces files in an existing Sunshine installation, and has not been accepted on the Junior PC. It is not a standalone installer or a reproducible build of this repository commit.
- Moonlight client: a matching password-capable Windows package is still required. Do not replace a working host during a stream or assume stock Moonlight supports this pairing mode.

## Source and dependencies

Clone with submodules:

```sh
git clone --recurse-submodules https://github.com/SlaVKsVolks/SunshineSeat.git
```

The 18 pinned submodules point to public upstream repositories. Windows builds use MSYS2 UCRT64; the source snapshot has not yet been rebuilt and accepted from its new commit. See [FORK-NOTES.md](FORK-NOTES.md) for provenance and deployment limits. Keep the existing host and client available for rollback during testing.

## License and attribution

SunshineSeat retains the upstream [GPL-3.0 license](LICENSE) and [notices](NOTICE). Upstream Sunshine documentation and releases are at [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine). This fork's source, candidate binary, and runtime acceptance have separate status.
