# SunshineSeat source snapshot

This is a custom Sunshine source snapshot with password-based Moonlight pairing and other SunshineSeat changes. It is not an official LizardByte release. The source was assembled from the local `codex/password-pairing` working tree on 2026-09-28 into a fresh repository without copying the working tree's Git history, machine logs, credentials, or deployment configuration. The upstream Sunshine GPL-3.0 license and copyright notices remain included.

New-device password pairing uses a password configured through the authenticated Sunshine web UI. A compatible password-capable Moonlight client is required; a stock Moonlight client must not be assumed to support this mode. Existing paired clients and host behavior still require installation and end-to-end verification on each machine.

This repository contains source code, not an installer. The previously staged `2026.928.1-password-pairing` runtime overlay is not part of this source snapshot. It has not been installed or accepted on the Junior PC, and its exact source provenance and matching Moonlight client package are still being checked. Do not replace a working Sunshine service based only on this repository.

The 18 public upstream submodule pins in `.gitmodules` are retained. A complete build needs those dependencies and the Windows toolchain described in the upstream project. The snapshot has not been rebuilt from its new commit yet; source publication is not a runtime acceptance claim.
