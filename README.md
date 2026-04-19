# claudochi

Flipper Zero app that turns the device into an Anthropic **Hardware Buddy** — a
tiny hardware companion for Claude Code Desktop. Pair it with the host's
Developer menu and the Flipper:

- shows live session state through an animated mascot
- fires a vibrating Pokemon-style "Wild Bash appeared!" modal when a tool wants
  permission, with d-pad navigation for ONCE / DENY
- evolves the mascot across five life stages as the session accumulates
  interactions — egg cracks, hatches, grows, ages into a little wizard
- runs a Tamagotchi layer underneath: play and food bars that respond to
  typing, tool activity, and approvals

Built as a pure userland FAP (`FlipperAppType.EXTERNAL`) against the
[Unleashed](https://github.com/DarkFlippers/unleashed-firmware) firmware — no
firmware fork, no reflash required.

Protocol spec:
[anthropics/claude-desktop-buddy REFERENCE.md](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md).

## What's in the latest release (v0.5.2)

### BLE + protocol
- Advertises as **Claude &lt;device-name&gt;** — pulled from your Flipper's
  Device Name setting (Settings → System → Device Name). Fresh Flippers use a
  preset (e.g. Nibblet); rename yours and that's what shows.
- LE Secure Connections bonded pairing — 6-digit passkey auto-rendered by the
  Flipper's built-in pin-code overlay.
- Nordic UART Service with RX (write + write-without-response) and TX (notify)
  characteristics at the canonical UUIDs.
- Heartbeat JSON parsed via vendored [jsmn](https://github.com/zserge/jsmn).
- Permission reply emits `{"cmd":"permission","id":"...","decision":"once"|"deny"}`
  — the only two decisions in REFERENCE.md.

### Pokemon-style prompt modal
When a tool wants permission, the prompt opens as a battle screen:

- **Banner**: `Wild <tool> appeared!`
- **Combat zone**: framed `?` glyph on the left vs. 32×32 attention pet on the
  right
- **Action row**: `ONCE | DENY` with cursor — Left/Right navigates, OK confirms
- **Narration**: `Claudochi used APPROVE!` (or `DENY!`) plays for 1.5 s after
  confirm before the modal closes

### Animated mascot with 14 states × 5 life stages
The mascot sprite is driven by a priority-ordered derivation from BT status +
heartbeat counts + local streak/tamagotchi state. 14 distinct states render
across 5 life stages = 70 animations total.

| State          | Trigger                                                   |
|----------------|-----------------------------------------------------------|
| `sleep`        | No BT connection, or nothing open on desktop              |
| `idle`         | Connected, sessions open but not generating               |
| `busy`         | `running > 0`                                             |
| `overloaded`   | `running > 0` AND `total >= 5`                            |
| `attention`    | `waiting > 0` OR permission modal open                    |
| `heart`        | Transient: approved a prompt within 5 s of it arriving    |
| `celebrate`    | Transient: 10k-token milestone, or stage evolution        |
| `reconnecting` | Transient: BT was Connected, dropped to Advertising       |
| `happy`        | 3+ consecutive approvals (mood)                           |
| `grumpy`       | 2+ consecutive denials (mood)                             |
| `content`      | Play + feed both high (long-horizon mood)                 |
| `focused`      | Feed high, play low — quietly at work                     |
| `lonely`       | Play + feed both low, sustained for 90 s                  |
| `starving`     | Feed == 0 for 5 min                                       |

Life stages switch at age thresholds (age is bumped on each classified
interaction):

| Stage | Age range | Look                                                |
|-------|-----------|-----------------------------------------------------|
| Egg   | 0..9      | Pristine → hairline crack (age 4) → fork (age 7)    |
| Child | 10..99    | Hatched, short boxy form                            |
| Teen  | 100..999  | Taller, small hair tuft                             |
| Adult | 1000..2999| Fuller form                                         |
| Elder | 3000+     | Wizard hat                                          |

When a stage boundary crosses, a 3-second full-screen cinematic plays with
radial starburst rays + baked-in Japanese text ("エボリュション / GETなんやぜ").
The backlight forces on at the start so you don't miss it.

### Tamagotchi loop
Two bars on the home screen:

- **Happy (♥)** — bumps on each classified msg change (capped at +15 per msg).
  Decays 2/min when idle.
- **Food (🍔)** — bumps from tokens-delta (every 80 tokens = +10 food, cap 100
  per heartbeat) AND +50 for each tool approval. Decays 1/min when idle.

Combined bar state derives mood: both high → `content`, food high + play low →
`focused`, both low for 90 s → `lonely`, zero food for 5 min → `starving`.

### Other niceties
- Blue LED solid while a central is connected.
- Vibro buzz + backlight-wake on prompt arrival.
- Clean teardown: restores the stock Serial BLE profile on app exit.
- **Konami code** on the home screen (Up Up Down Down Left Right Left Right)
  resets the pet to a fresh egg.
- Persistent state on SD card at `/ext/apps_data/claude_buddy/state.bin` —
  the pet remembers age, feed, play, and lifetime approvals across reboots.

## Requirements

- **Flipper Zero running Unleashed firmware** (any recent `unlshd-*` release).
  Flash via [qFlipper](https://flipperzero.one/update) or the
  [Unleashed web updater](https://unleashedflip.com/).
- **Claude Code Desktop** with Hardware Buddy enabled. Currently gated by a
  server-side flag; if you don't see `Developer → Open Hardware Buddy…` after
  enabling Developer Mode, the feature isn't turned on for your account yet.
- **macOS or Windows host.** On macOS, grant Claude the Bluetooth permission
  (System Settings → Privacy & Security → Bluetooth) — this was the single
  biggest gotcha during development.
- `pipx` for installing `ufbt` (build tool).

## Install from release

Download `claude_buddy.fap` from the
[latest release](https://github.com/PatrionDigital/claudochi/releases/latest)
and drop it into `SD Card/apps/Bluetooth/` via qFlipper. Launch from
**Apps → Bluetooth → Claude Buddy**.

## Build from source

```sh
# Install ufbt and pin the SDK channel to Unleashed
pipx install ufbt
ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release

# Clone and build
git clone https://github.com/PatrionDigital/claudochi.git
cd claudochi/claude_buddy
ufbt launch    # builds + uploads + launches in one step
```

Build output lands at `claude_buddy/dist/claude_buddy.fap`.

## Pairing flow

1. Launch the Claude Buddy app on the Flipper — home screen shows the mascot
   in the sleep state (BT not yet connected).
2. On your Mac/PC, open **Claude Code Desktop**.
3. **Help → Troubleshooting → Enable Developer Mode**. Fully quit (cmd+Q) and
   relaunch so the Developer menu registers.
4. **Developer → Open Hardware Buddy…**
5. Click **Connect**. Pick `Claude <your-device-name>` from the scan list.
6. macOS/Windows shows a system-level pairing prompt. The Flipper's screen
   overlays a 6-digit passkey; type it into the host prompt.
7. Done. Blue LED on, mascot shifts to idle, and the Tamagotchi loop starts
   reacting to your session.

## Troubleshooting

- **Claude Desktop's Hardware Buddy picker shows "None found".** Most likely
  the macOS Bluetooth permission for Claude isn't granted. Check System
  Settings → Privacy & Security → Bluetooth. Second-most-likely: stale
  CoreBluetooth scan cache — `sudo pkill bluetoothd` and reopen the Hardware
  Buddy window. Third: `Developer → Open Hardware Buddy…` is missing entirely
  — the Hardware Buddy feature gate isn't enabled on your account yet.
- **`ufbt launch` hangs at `Using flip_<name>`.** The Flipper's display has
  gone to sleep; the CLI prompt task is descheduled and the byte ufbt is
  waiting for never arrives. Press any button to wake, retry immediately. If
  no response, hold Back for ~5 seconds to force-reboot.
- **Build errors about undefined BLE symbols.** The SDK is pointing at stock
  Flipper instead of Unleashed. Re-run the `ufbt update` command from the
  Build section.
- **Pet seems stuck on one state or BT disconnects repeatedly.** Clean-install
  cycle: qFlipper File Manager → delete `SD/apps/Bluetooth/claude_buddy.fap`
  and `SD/apps_data/claude_buddy/`. On Mac: System Settings → Bluetooth →
  Forget `Claude <device>`. Re-drop the .fap, relaunch, reconnect.

## Project layout

```
claudochi/
├── claude_buddy/
│   ├── application.fam                 # FAP manifest, version, icon, assets
│   ├── icons/claude_buddy_10px.png     # Apps-menu icon (10×10 1-bit)
│   ├── assets/                         # in-app sprite bundle
│   │   ├── mascot_<stage>_<state>_64x64/   # 70 animations (5 stages × 14 states)
│   │   ├── mascot_<stage>_attn_32x32.png   # 5 modal-sized attention sprites
│   │   ├── mascot_evolution_128x64/        # full-screen stage cinematic
│   │   ├── icon_heart_10x10.png            # Happy bar glyph
│   │   ├── icon_burger_10x10.png           # Food bar glyph
│   │   ├── icon_tool_32x32.png             # modal "enemy tool" glyph
│   │   ├── overlay_egg_crack_*              # progressive egg cracks
│   │   └── overlay_{charging,hungry}_8x8    # power-state corner overlays
│   ├── claude_buddy.c                  # main — GUI, state machine, RX drain, Tamagotchi
│   ├── claude_buddy_profile.{h,c}      # BLE profile (NUS GATT + advertising)
│   ├── ble_stack_shim.h                # vendored HCI packet types + VSEVT codes
│   └── jsmn.h                          # vendored MIT-licensed JSON parser
├── reference/                          # shallow Unleashed clone (gitignored)
├── TODO.md                             # living backlog
└── README.md
```

## Protocol coverage vs REFERENCE.md

Implemented:
- [x] NUS service + RX (write + write-without-response) + TX (notify) at canonical UUIDs
- [x] Device name starting with `Claude`
- [x] Bonded pairing with 6-digit passkey (DisplayOnly IO capability)
- [x] Heartbeat snapshot parsing: `total`, `running`, `waiting`, `msg`, `tokens`, `prompt`
- [x] Permission reply: `{"cmd":"permission","id":"...","decision":"once"|"deny"}`

Not yet (see [TODO.md](TODO.md)):
- [ ] `{"cmd":"unpair"}` handler — desktop "Forget" button
- [ ] Time sync / owner name rendering
- [ ] Turn events (per-turn token reporting → special animations)
- [ ] Folder push (1.8 MB asset transport for custom sprite packs)

## License

TBD.
