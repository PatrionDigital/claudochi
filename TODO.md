# raderado — project backlog

Loose prioritization. Top groups are in rough "most likely next" order; mark items with a priority tag when we decide (P0/P1/P2) so the Now section stays honest.

## Shipped (v0.x)

- [x] BLE profile: advertising, LE Secure Connections + MITM-bonded pairing, 6-digit passkey overlay
- [x] NUS GATT: RX (write + write-without-resp), TX (notify) characteristics, full end-to-end with Claude Desktop
- [x] Heartbeat parse (jsmn), msg + counters rendered live
- [x] Prompt modal with hardware approve/deny — validated on real Claude Code Desktop
- [x] Blue LED on connected, vibro buzz on modal open
- [x] Teardown restores stock Serial profile

## Now

*(nothing actively in progress)*

## Next up — low-hanging polish

- [ ] **FAP icon** (~15 min). 10x10 1-bit PNG, add `fap_icon="claude_buddy_10px.png"` to application.fam. Stylized silhouette of the Claude Code mascot creature (the terracotta boxy robot character — see reference image in commit history or `docs/mascot_reference.png` if saved).
  - Source: research confirms format at `reference/scripts/fbt/elfmanifest.py:56-63`
- [ ] **Summarize `msg` to ≤2 words for display**. Current heartbeat `msg` field is "running: yarn test" / "approve: Bash" / free-form text from the desktop. Truncates awkwardly on the 128-px Flipper screen. Map to short tokens — e.g. "RUN", "WAIT", "DONE", "ASK" — either by local string matching, or by asking the desktop for a short form (Phase 5 protocol ask)
- [ ] **Tag v0.1 release** on GitHub
- [ ] **README rewrite** — current one has speculative troubleshooting from the debugging thrash. Replace with the actual validated flow + screenshots of the Flipper UI in each state
- [ ] **Show "reconnecting..." on Flipper when BT drops** instead of silently falling back to "Advertising"

## Animated mascot

- [ ] **Character state machine** derived from heartbeat signals
  - sleep: `total == 0` for N seconds
  - idle: `total > 0 && running == 0`
  - busy: `running > 0`
  - attention: `waiting > 0` (prompt pending)
  - celebrate: every N tokens crossed (per REFERENCE.md's "every 50K tokens")
- [ ] **Asset pipeline**: `claude_buddy/assets/<state>/frame_NN.png` + `frame_rate`. Add `fap_icon_assets="assets"` / `fap_icon_assets_symbol="claude_buddy"` to application.fam
- [ ] **Replace 4 Hz redraw timer** with `view_tie_icon_animation`'s per-frame hook
  - Pattern reference: `reference/applications/main/lfrfid/views/lfrfid_view_read.c:29-109`
- [ ] **v1 art**: one simple mascot × 5 states — aim for ~5-8 frames per state, 48×48 px, 4-8 FPS
- [ ] **Multi-pet support** (v2+): port the "18 pets × 7 anims" concept from the ESP32 reference. Gate on `preference` key TBD. Could be "each Flipper picks a pet at first pair, stored in furi_hal prefs"

## Tamagotchi layer

The desktop's heartbeat `msg` field already streams the user's input and tool activity in near-real time. That's a rich signal we haven't tapped — currently it's rendered as plain text. The idea: let that stream *care for* the pet. Mechanics sketch, to be refined when we get here:

- [ ] **Needs decay over time.** Hunger, happiness, energy, bond. Each drops by a small amount per minute when the desktop is idle.
- [ ] **Interactions refill needs.**
  - User-input activity (msg length, rate of change) → **attention / bond**
  - Token accumulation (`tokens_today`) → **fullness / hunger ↓**
  - Approved tool calls → **happiness** (denied = small stress)
  - Time with active session (`total > 0`) → **energy** (or exhaustion if sustained)
- [ ] **Keyword reactions.** Cheap text scan on `msg` to trigger one-off animations:
  - Exclamation marks / "!" count → startled or excited
  - "please" / "thanks" → happy wiggle
  - "fix" / "bug" / "error" → concerned face
  - Profanity / frustration markers → covers ears / hides
  - Food words ("lunch", "coffee") → eating animation
  - Easter-egg triggers (birthday, project milestones) → celebrate
- [ ] **Mood state derived from needs.** Not just the raw heartbeat. E.g. happy pet + low energy = yawning-idle; high attention = playful-idle
- [ ] **Personality seed.** Derive from the Flipper's BD_ADDR on first pair — each device gets stable "trait weights" that bias which reactions fire. Two Flippers running the same firmware feel subtly different
- [ ] **Persistence.** Save needs + personality to SD card every few minutes; load on app start. Pet survives reboots with memory intact
- [ ] **Randomness layer.** Base state + weighted-random variant frames. Occasional "thinking", "scratching", "looking around" idle fidgets so the pet feels alive instead of looped
- [ ] **Neglect consequences.** After prolonged idle without any desktop activity, pet gets sadder / dirtier / goes to sleep. Comes back happy on next heartbeat burst. Gives the coming-back-to-your-desk moment emotional weight
- [ ] **Visible stats UI** (toggleable second view): current needs as 4 bars, recent triggers log, total "care score." Pure diagnostics, not required for the core loop

## Protocol completeness (vs REFERENCE.md)

- [ ] **Richer permission decisions**: we currently send only `once` and `deny`. Spec also supports `session` and `always`. Add a second input mode (long-press or Up/Down cycle) to select decision type before confirming
- [ ] **`{"cmd":"unpair"}` handler** — desktop sends this when user clicks "Forget" on its side. We should clear our bond and restart pairing
- [ ] **Time sync** — desktop sends wall-clock timestamp, device can use it for display
- [ ] **Owner name** — desktop publishes the paired user's name; could show on our idle screen
- [ ] **Turn event reporting** — per REFERENCE.md, desktop fires a turn-complete event with tool/token data. Could animate "celebrate" state on big turns
- [ ] **Folder push** — 1.8MB transport for custom pet packs. Parses chunked frames. Big job; only worth it when we have the multi-pet selection UX

## Diagnostics + stats

- [ ] **Stats panel** (second view) — total tokens, tokens_today, session count, uptime. Triggered by Up/Down on home screen
- [ ] **BLE signal strength indicator** — read RSSI via GAP, show as 1-4 bar icon
- [ ] **Power management** — dim the screen after N seconds idle, wake on next heartbeat
- [ ] **Persistent bond logs** — write connect/disconnect events to SD for postmortem

## Infrastructure

- [ ] **Remove Phase-1 diagnostic statements** once we're confident. E.g. `FURI_LOG_I(TAG, "NUS service up...")` can go, and the "sent: once (ok)" msg overwrite could be silenced after some duration instead of staying until next heartbeat
- [ ] **CI**: GitHub Actions workflow that runs `ufbt` on push to verify the FAP still builds cleanly against pinned Unleashed SDK
- [ ] **Automated release pipeline**: tag push → build `.fap` → attach to GitHub release
- [ ] **SDK version pin**: commit a `.ufbt/state` or similar so `ufbt update` reproducibly lands on the same Unleashed release
- [ ] **Unit tests for the JSON parser path** — feed synthetic heartbeats and verify state transitions without needing a real BLE link

## Out of scope for now

- Flipper Momentum / RogueMaster firmware support — test on those after v0.1 is shipped
- Windows testing — no Windows box handy
- Non-Flipper-Zero hardware targets (nRF52, etc.) — the point is the Flipper port
