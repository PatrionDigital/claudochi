# claudochi

Flipper Zero app that turns the device into an Anthropic **Hardware Buddy** —
a tiny hardware companion for Claude Code Desktop. Pair it with the host's
Developer menu, and the Flipper shows live session state, fires a vibrating
modal when Claude asks for permission, and lets you approve or deny with a
physical button. Feeds a lightweight tamagotchi layer along the way.

Built as a pure userland FAP (`FlipperAppType.EXTERNAL`) against the
[Unleashed](https://github.com/DarkFlippers/unleashed-firmware) firmware —
no firmware fork, no reflash required.

Protocol spec: [anthropics/claude-desktop-buddy REFERENCE.md](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md).

## What works (v0.1)

- Advertises as **Claude &lt;device-name&gt;** — pulls from your Flipper's
  Device Name setting (Settings → System → Device Name). Fresh Flippers
  use a preset (e.g. Nibblet); if you renamed yours, that's what shows.
- LE Secure Connections bonded pairing — 6-digit passkey auto-rendered by the
  Flipper's built-in pin-code overlay
- Nordic UART Service with RX (write) and TX (notify) characteristics at the
  canonical UUIDs
- Heartbeat JSON parsed via vendored jsmn, rendered live on the 128×64 OLED
- **Four pet states** derived from heartbeat signals, each with its own 64×64
  1-bit sprite:

  | State | Trigger | Visual |
  |---|---|---|
  | `sleep` | No BT connection, or nothing open on desktop | Closed eyes, `Zzz` trail drifting up |
  | `idle` | Connected, `total > 0`, `running == 0` | Base pose, neutral face |
  | `busy` | `running > 0` (Claude generating) | Focused pupils, thinking-dots mouth, three dots floating above head |
  | `attention` | `waiting > 0` or permission modal open | Wide eyes, open mouth, big `!` above head |

- Permission prompt modal with hardware button decisions:
  - **OK** emits `{"cmd":"permission","id":"...","decision":"once"}` over TX
  - **Left** emits `decision:"deny"`
  - Vibrates on modal open (edge-triggered, doesn't buzz repeatedly)
  - Modal layout: attention mascot on the left, tool/hint/keybindings on the right
- Blue LED solid while a central is connected
- Clean teardown: restores the stock Serial BLE profile on exit

## Requirements

- **Flipper Zero running Unleashed firmware** (any recent `unlshd-*` release).
  Flash via [qFlipper](https://flipperzero.one/update) or the
  [Unleashed web updater](https://unleashedflip.com/).
- **Claude Code Desktop** with the Hardware Buddy feature enabled on your
  account. Currently gated by a server-side flag; request access through
  Anthropic's maker/support channels if you don't see the `Developer → Open
  Hardware Buddy…` menu item after enabling Developer Mode.
- **macOS or Windows host.** On macOS, grant Claude the Bluetooth permission
  (System Settings → Privacy & Security → Bluetooth) — this was the single
  biggest gotcha during dev.
- `pipx` for installing `ufbt` (build tool).

## Setup

```sh
# Install ufbt and pin the SDK channel to Unleashed
pipx install ufbt
ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release

# Clone this repo
git clone https://github.com/PatrionDigital/claudochi.git
cd claudochi
```

## Build and install

### Dev loop — build, push, launch in one step

```sh
cd claude_buddy
ufbt launch
```

Compiles `claude_buddy.fap`, uploads to a temporary SD path, and launches.
Ephemeral; fastest iteration path.

### Persistent install

Build output lands at `claude_buddy/dist/claude_buddy.fap`. To make it
appear permanently in the Flipper's Apps menu:

1. Plug the Flipper in, open **qFlipper**
2. File Manager → `SD Card/apps/Bluetooth/`
3. Drop `claude_buddy.fap` in

Launch from the Flipper: **Apps → Bluetooth → Claude Buddy**.

## Pairing flow

1. Launch the Claude Buddy app on the Flipper — screen shows the mascot and
   `BT: Advertising`.
2. On your Mac/PC, open **Claude Code Desktop**.
3. **Help → Troubleshooting → Enable Developer Mode**. Fully quit (cmd+Q)
   and relaunch so the Developer menu registers.
4. **Developer → Open Hardware Buddy…**
5. Click **Connect**. Pick `Claude <your-device-name>` from the scan list.
6. macOS/Windows shows a system-level pairing prompt. The Flipper's screen
   overlays a 6-digit passkey; type it into the host prompt.
7. Done. Flipper BT line flips to `Connected`, blue LED on, mascot starts
   reacting to your session.

## Troubleshooting

- **Claude Desktop's Hardware Buddy picker shows "None found".** Most
  likely the macOS Bluetooth permission for Claude isn't granted. Check
  System Settings → Privacy & Security → Bluetooth. Second-most-likely:
  stale CoreBluetooth scan cache; `sudo pkill bluetoothd` and reopen the
  Hardware Buddy window. Third: `Developer → Open Hardware Buddy…` menu
  item is missing entirely — the Hardware Buddy feature gate isn't enabled
  on your account yet (ask Anthropic).
- **`ufbt launch` hangs at `Using flip_<name>`.** The Flipper's display has
  gone to sleep; its CLI prompt task is descheduled and the prompt byte
  ufbt is waiting for never arrives. Press any button to wake, retry
  immediately. If pressing buttons doesn't wake it, force-reboot by
  holding Back for ~5 seconds.
- **LightBlue sees the peripheral but Claude Desktop doesn't.** Permission
  or cache issue as above. The picker filters its own scan by name prefix
  (`Claude` / `Nibblet`), not by service UUID.
- **Build errors about undefined BLE symbols.** The SDK is pointing at
  stock Flipper instead of Unleashed. Re-run the `ufbt update` command
  from the Setup section.
- **"None found" even after all of the above.** Try `ufbt launch` a second
  time — occasionally the Flipper's BLE stack needs a restart to recover
  from a previous broken adv state.

## Project layout

```
claudochi/
├── claude_buddy/
│   ├── application.fam                 # FAP manifest, icon + assets wired
│   ├── icons/claude_buddy_10px.png     # Apps-menu icon (10×10 1-bit)
│   ├── assets/                         # in-app sprite bundle
│   │   ├── mascot_idle_64x64.png
│   │   ├── mascot_sleep_64x64.png
│   │   ├── mascot_busy_64x64.png
│   │   └── mascot_attention_64x64.png
│   ├── claude_buddy.c                  # main — GUI, state machine, RX drain
│   ├── claude_buddy_profile.{h,c}      # BLE profile (NUS GATT + advertising)
│   ├── ble_stack_shim.h                # vendored HCI packet types + VSEVT codes
│   └── jsmn.h                          # vendored MIT-licensed JSON parser
├── reference/                          # shallow Unleashed clone (gitignored)
├── TODO.md                             # living backlog
└── README.md
```

## Protocol coverage vs REFERENCE.md

Implemented:
- [x] NUS service + RX (write + write-without-response) + TX (notify) at the canonical UUIDs
- [x] Device name starting with `Claude`
- [x] Bonded pairing with 6-digit passkey (DisplayOnly IO capability)
- [x] Heartbeat snapshot parsing: `total`, `running`, `waiting`, `msg`, `prompt`
- [x] Permission reply: `{"cmd":"permission","id":"...","decision":"once"|"deny"}`

Not yet:
- [ ] `{"cmd":"unpair"}` handler
- [ ] Richer permission decisions: `session`, `always`
- [ ] Time sync / owner name rendering
- [ ] Turn events (per-turn token reporting)
- [ ] Folder push (1.8 MB asset transport for custom sprite packs)

See [TODO.md](TODO.md) for the full roadmap.

## License

TBD.
