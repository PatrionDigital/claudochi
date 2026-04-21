# CLAUDE.md

Project-specific context for Claude Code sessions working in this repo.

## Canonical local path

`~/Projects/RADERADO/`

The GitHub repo is `PatrionDigital/claudochi`; the local folder is named
`RADERADO` for historical reasons (it was my Flipper Zero's device name
before the rename). The disk location is on internal HDD, not in any
iCloud-synced directory — keep it that way; iCloud syncing intermittently
blocks file moves and breaks native-binding loads during builds.

## What this is

Flipper Zero FAP that turns the device into an Anthropic "Hardware Buddy"
bridge for Claude Code Desktop. Core features:

- Pokemon-style prompt modal with "Wild <tool> appeared!" banner and
  narrated `Claudochi used APPROVE!/DENY!` decision phase
- Tamagotchi mascot — 14 states × 5 evolution stages = 70 animations
- Full-screen evolution cinematic on stage crossings + backlight wake
- Tokens→feed, approvals→feed, msg-changes→play, all with decay
- Konami reset (d-pad)
- Full REFERENCE.md protocol coverage except `cmd:name`, `cmd:status`,
  and folder-push transport

See `README.md` for user-facing feature list, `TODO.md` for backlog.

## Build

```sh
# First run in a new clone:
./scripts/sdk-pin.sh

# Iterate:
cd claude_buddy
ufbt launch    # build + upload + launch via USB
ufbt           # build only → dist/claude_buddy.fap
```

SDK is pinned via URL + SHA256 in `scripts/sdk-pin.sh`. Bump instructions
in that script's header. CI uses the same script so dev and CI can't
diverge.

## Release flow (automated)

Tag push triggers `.github/workflows/release.yml` — builds FAP against
the pinned SDK, creates a GitHub release using the tag annotation as the
release body, attaches the FAP. Flow:

```sh
git tag -a v0.X.Y -m "v0.X.Y — summary

Longer notes go here; they become the release body."
git push origin v0.X.Y
# CI does the rest. Do NOT manually `gh release create` — it'll conflict.
```

## SemVer convention

- **patch** (x.y.Z+1): bug fix, tuning, cleanup, doc, infra. No new
  user-facing feature.
- **minor** (x.Y+1.0): new user-facing feature.
- **major** (X+1.0.0): reserved for stable-API declaration or a protocol
  break.

Each incremental commit during a feature's rebuild typically bumps patch
(v0.5.3, v0.5.4, ...); the final feature-complete commit that ends the
series can bump minor and tag (v0.6.0).

## Discipline that stuck (learned the hard way)

1. **Small validated increments.** Each version bump is ONE focused
   change, flashed + on-device-validated before the next lands on main.
   v0.5.0's thrash — bundling modal rewrite + speculative protocol
   additions + feed tuning + debug logs into one commit — caused a
   multi-hour debugging cycle and a full revert. The v0.4.x rebuild
   (one logical change per patch) was night-and-day smoother and
   shipped faster overall.
2. **Protocol fidelity — read REFERENCE.md first.** `session`/`always`
   decisions were speculatively added in v0.5.0 and silently dropped by
   the desktop, causing phantom modal reopens. Don't extend the protocol
   on guess. The spec is at
   <https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md>
3. **On-device validation gates merges.** `ufbt launch` → exercise the
   affected path → user confirms before proceeding to the next
   increment.
4. **Clean install when things get weird.** Delete
   `SD/apps/Bluetooth/claude_buddy.fap`, `SD/apps_data/claude_buddy/`,
   forget the macOS bond, `sudo pkill bluetoothd`, flash fresh. Solves
   most inexplicable "last update broke something" reports.

## Key pointers

- **Protocol spec (authoritative)**:
  <https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md>
- **Target firmware**: Unleashed,
  <https://github.com/DarkFlippers/unleashed-firmware>
- **Vendored**: `claude_buddy/jsmn.h` (MIT JSON parser),
  `claude_buddy/ble_stack_shim.h` (HCI packet types)
- **Gitignored reference clone**: `reference/` — fetch for grepping SDK
  source:
  `git clone --depth=1 https://github.com/DarkFlippers/unleashed-firmware.git reference`

## Gitignored build outputs

- `claude_buddy/dist/claude_buddy.fap` — release build
- `claude_buddy/.ufbt/` — local ufbt state (symlinks, build cache)
- `claude_buddy/build/`, `claude_buddy/.vscode/` — intermediate

## Commit trailer

Agent-authored commits end with:

```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```
