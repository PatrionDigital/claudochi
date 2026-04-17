# raderado

Flipper Zero FAP that acts as a BLE peripheral for Anthropic's Claude desktop
Hardware Buddy bridge. Built as a pure userland app (`FlipperAppType.EXTERNAL`)
— no firmware fork required.

Target firmware: [Unleashed](https://github.com/DarkFlippers/unleashed-firmware).
Protocol spec: [anthropics/claude-desktop-buddy REFERENCE.md](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md).

**Status:** Phase 1 skeleton. Profile template, GAP advertising config, and
NUS UUIDs are in place. GATT service/characteristic wiring and JSON handling
are the next milestone.

## Requirements

- Flipper Zero running **Unleashed firmware** (any recent `unlshd-*` release).
  Install via [qFlipper](https://flipperzero.one/update) or the
  [Unleashed web updater](https://unleashedflip.com/).
- macOS / Linux / Windows with Python 3.10+ and `pipx` (recommended) or
  `pip --user`.
- USB-C cable connecting the Flipper to your computer for dev flashing.

## One-time setup

```sh
# 1. Install ufbt (Flipper's SDK-based build tool)
pipx install ufbt

# 2. Pin the SDK to the Unleashed release channel
#    (stock update.flipperzero.one does NOT export the symbols this app needs)
ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release

# 3. Clone this repo
git clone https://github.com/PatrionDigital/raderado.git
cd raderado
```

## Build and install

### Dev loop — build, push, launch in one step

Plug the Flipper in over USB, close qFlipper if it's open (it holds the serial
port), then:

```sh
cd claude_buddy
ufbt launch
```

`ufbt launch` compiles `claude_buddy.fap`, uploads it to the Flipper's SD
card under a temporary path, and launches it immediately. This is the fastest
iteration path — the app is not retained across reboots.

### Persistent install

The build artifact lives at `claude_buddy/dist/claude_buddy.fap`. To install
it permanently so it shows up in the Flipper's Apps menu:

1. Plug the Flipper in and open **qFlipper**.
2. Open the File Manager tab.
3. Navigate to `SD Card/apps/Bluetooth/` (matches the `fap_category="Bluetooth"`
   declared in [application.fam](claude_buddy/application.fam)).
4. Drag `claude_buddy/dist/claude_buddy.fap` into that folder.

On the Flipper: **Apps → Bluetooth → Claude Buddy** to launch.

### Alternative: via CLI

```sh
cd claude_buddy
ufbt                            # build only
ufbt cli                        # opens Flipper CLI over USB
# then at the Flipper prompt:
storage write_chunk /ext/apps/Bluetooth/claude_buddy.fap <size>
# (or use 'storage mkdir' and 'storage write' as appropriate — qFlipper is simpler)
```

## Pairing with the Claude desktop app

1. On your Mac/Windows: open the Claude desktop app.
2. **Help → Troubleshooting → Enable Developer Mode** (macOS menu bar), then
   fully quit (`cmd+Q`) and relaunch.
3. **Developer → Open Hardware Buddy…** — if this item is missing, the
   feature is gated on your account; see *Troubleshooting* below.
4. Click **Connect**. The picker lists `Claude` followed by two hex
   digits derived from the BT MAC's low byte (e.g. `Claude4F`). Eight
   characters is the maximum that fits in a 31-byte legacy adv packet
   alongside the 128-bit NUS UUID and flags.
5. Select it. macOS will ask for Bluetooth permission on first use.
6. The Flipper screen will show a **6-digit passkey** (rendered by the
   bt_service's built-in pin-code overlay). Type it into the desktop
   prompt. Bonding + encryption are established; the desktop then
   subscribes to TX notifications and begins sending heartbeat JSON.

The link is LE Secure Connections with MITM protection by default —
`CLAUDE_BUDDY_ENCRYPTED` is set in [application.fam](claude_buddy/application.fam).
Rebuild with that cdefine removed to regress to an unencrypted link
for diagnostics (see the `#ifdef` branch in
[claude_buddy_profile.c](claude_buddy/claude_buddy_profile.c)).

## Project layout

```
raderado/
├── claude_buddy/                     # the FAP
│   ├── application.fam               # FAP manifest
│   ├── claude_buddy.c                # main entry point
│   ├── claude_buddy_profile.{c,h}    # BLE profile template, NUS UUIDs, GAP config
│   └── dist/claude_buddy.fap         # build output (gitignored)
├── reference/                        # shallow clone of Unleashed firmware (gitignored, read-only)
└── README.md
```

## Troubleshooting

- **`ufbt launch` hangs at `Using flip_<name>...` and never progresses.**
  The Flipper's CLI only emits its prompt when the display is awake and you're
  at the home/dolphin screen. Wake the screen with any button press and retry
  immediately. If the screen is frozen, force-reboot by holding **Back** for
  ~5 seconds. If the port itself is held, `lsof /dev/cu.usbmodemflip_*` on
  macOS will show the offending process (usually qFlipper) — quit it.
- **Build errors about undefined BLE symbols.** The SDK channel is wrong —
  re-run `ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release`.
- **App builds but the desktop picker doesn't see "Claude-Raderado".** Three
  possibilities: (a) the Flipper's BT service is off — enable it in Settings →
  Bluetooth; (b) the mobile app still has the Serial profile bonded — forget
  the pairing on your phone; (c) Developer → Open Hardware Buddy… never found
  it — account-level gate, see next entry.
- **Developer menu exists but "Open Hardware Buddy…" is missing.** The feature
  is gated by a server-side GrowthBook flag (ID `2358734848` as of April
  2026). Editing local config won't enable it; the menu is built once from
  the initial flag fetch. Ask Anthropic maker/hardware support to enable it
  on your account. Runtime overrides in DevTools (`Op["2358734848"]={on:true}`
  or `window.__growthbook.setForcedFeatures(...)`) don't re-trigger the menu
  build.

## License

TBD.
