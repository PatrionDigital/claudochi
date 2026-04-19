# claudochi — project backlog

Loose prioritization. Top groups are in rough "most likely next" order; mark items with a priority tag when we decide (P0/P1/P2) so the Now section stays honest.

## Shipped (v0.x)

### v0.1 — BLE + prompts + mascot
- [x] BLE profile: advertising, LE Secure Connections + MITM-bonded pairing, 6-digit passkey overlay
- [x] NUS GATT: RX (write + write-without-resp), TX (notify) characteristics, full end-to-end with Claude Desktop
- [x] Heartbeat parse (jsmn), msg + counters rendered live
- [x] Prompt modal with hardware approve/deny — validated on real Claude Code Desktop
- [x] Blue LED on connected, vibro buzz on modal open
- [x] Teardown restores stock Serial profile
- [x] FAP icon (10x10 mascot silhouette)
- [x] Character state machine (sleep/idle/busy/attention) from heartbeat
- [x] Asset pipeline wired (`fap_icon_assets="assets"`, `_symbol="claude_buddy"`)
- [x] Static 64×64 sprite per state with contextual overlays
- [x] Summarize `msg` to a short label + detail (RUN/ASK/DONE/FAIL/OK/NO)
- [x] Reconnecting transient on BT drop
- [x] Stop rendering free-form msg text (privacy)

### v0.2 — animated mascot + 6 new states
- [x] Phase 5d animated frames (2-6 per state)
- [x] Phase 5e.1 Claudegotchi play/feed mechanics (split-A classifier)
- [x] Phase 5e.2 SD persistence + age + H/F bars + T age readout
- [x] Phase 5f: 6 new mascot states (heart/celebrate/reconnecting/happy/grumpy/overloaded) + overlay icons + backlight-on-prompt
- [x] Phase 5g: 5 life stages × 14 states = 70 animations
- [x] Evolution thresholds calibrated (~1 month of 100msgs/day to Elder)

### v0.3 — Claudochi polish + feed tuning
- [x] Repo renamed raderado → claudochi (device-agnostic branding)
- [x] Title "Claude" → "Claudochi"
- [x] 10x10 heart + hamburger glyphs in two-row stat layout
- [x] Konami code reset (Up Up Down Down Left Right Left Right) with freeze fix
- [x] v0.3.1: tokens-delta → feed direct path
- [x] Phase 5h: egg-crack progression within Egg stage (ages 4-9)
- [x] v0.3.3: approvals feed the pet (+50 per OK)
- [x] v0.3.4: cap per-msg play/feed at 15
- [x] v0.3.5: retune tokens→feed rate for real session pace
- [x] GitHub release tagged, repo public, .fap attached as asset

### v0.4 — full-screen evolution cinematic
- [x] Full-screen evolution cinematic fires on stage-age threshold crossing (Egg→Child @10, Child→Teen @100, Teen→Adult @1000, Adult→Elder @3000)
- [x] Generic 12-frame starburst animation with baked-in Japanese text ("エボリュション / GETなんやぜ")
- [x] Post-revert rebuild sequence: v0.4.1 (feed tune) → v0.4.2 (32×32 sprites) → v0.4.3 (Pokemon modal layout) → v0.4.4 (narration)

### v0.5 — Pokemon-attack-style prompt modal
- [x] Pokemon battle-screen prompt modal: "Wild <tool> appeared!" banner, framed "?" glyph vs. 32×32 attention pet, ONCE | DENY action row
- [x] Left/Right cursor nav, OK confirms selection
- [x] Narration phase "Claudochi used APPROVE!/DENY!" for 1.5s after confirm
- [x] v0.5.1: evolution anim text contrast fix (white rect behind glyphs)
- [x] v0.5.2: force backlight on at evolution cinematic start

## Now

*(nothing actively in progress)*

## Next up — low-hanging polish

- [ ] **README rewrite** — current README has stale "BT: Advertising/Connected" references and pre-Pokemon-modal flow. Replace with validated flow + screenshots of the five life stages and the battle modal.
- [ ] **Remove Phase-1 diagnostic statements** — `FURI_LOG_I(TAG, "NUS service up...")` etc. Silence the "sent: once (ok)" `hb_msg` overwrite after a short linger rather than holding it until the next heartbeat.
- [ ] **Richer `summarize_msg` ladder** — observe real desktop msg strings during a session (FURI_LOG on unlabeled msgs), extend pattern list so age-bump rate reflects only meaningful interactions, not every streaming status.

## Infrastructure

- [ ] **SDK version pin**: commit `.ufbt/state` (or similar) so `ufbt update` reproducibly lands on the same Unleashed release.
- [ ] **CI**: GitHub Actions workflow that runs `ufbt` on push to verify the FAP still builds cleanly against the pinned SDK.
- [ ] **Automated release pipeline**: tag push → build `.fap` → attach to GitHub release automatically.
- [ ] **Unit tests for the JSON parser path** — feed synthetic heartbeats and verify state transitions without needing a real BLE link.

## Medium features

- [ ] **Stats panel** (toggle second view): total tokens, tokens_today, session count, uptime, approval/denial lifetime counters. Triggered by Up or Down on home screen.
- [ ] **BLE signal strength indicator** — read RSSI via GAP, show as a 1–4 bar icon in the corner.
- [ ] **Power management** — dim the screen after N seconds idle, wake on next heartbeat or prompt.
- [ ] **Persistent bond logs** — write connect/disconnect events to SD for postmortem analysis.

## Protocol completeness (vs REFERENCE.md)

- [ ] **`{"cmd":"unpair"}` handler** — desktop sends this when user clicks "Forget" on its side. Clear our bond and restart pairing.
- [ ] **Time sync** — desktop sends wall-clock timestamp, device can use it for display.
- [ ] **Owner name** — desktop publishes the paired user's name; show on idle screen.
- [ ] **Turn event reporting** — per REFERENCE.md, desktop fires a turn-complete event with tool/token data. Animate "celebrate" state on big turns.
- [ ] **Folder push** — 1.8 MB transport for custom pet packs. Chunked-frame parser. Big job; only worth it when multi-pet selection UX exists.

### Deferred pending protocol additions
The following *are not in the current REFERENCE.md spec* but have been written as speculative hooks. Re-scope only if/when the protocol adds them:
- ~~Session / Always permission decisions~~ — desktop supports only `once` and `deny`. Speculatively adding `session`/`always` in v0.5.0 caused a phantom modal-reopen bug; don't retry without protocol confirmation.

## Tamagotchi layer (beyond what v0.2–v0.5 shipped)

The desktop's heartbeat `msg` field already streams user input and tool activity in near-real time. v0.2–v0.5 wired play/feed/age + 5 life stages + mood derivation. Remaining ideas:

- [ ] **Keyword reactions.** Cheap text scan on `msg` to trigger one-off animations:
  - Exclamation marks / "!" count → startled or excited
  - "please" / "thanks" → happy wiggle
  - "fix" / "bug" / "error" → concerned face
  - Profanity / frustration markers → covers ears / hides
  - Food words ("lunch", "coffee") → eating animation
  - Easter-egg triggers (birthday, project milestones) → celebrate
- [ ] **Richer mood derivation** — combine needs with time-of-day or energy state. E.g. happy pet + low energy = yawning-idle; high attention = playful-idle.
- [ ] **Personality seed.** Derive from the Flipper's BD_ADDR on first pair — each device gets stable "trait weights" that bias which reactions fire. Two Flippers running the same firmware feel subtly different.
- [ ] **Randomness layer.** Base state + weighted-random variant frames. Occasional "thinking", "scratching", "looking around" idle fidgets so the pet feels alive instead of looped.
- [ ] **Neglect consequences.** After prolonged idle without any desktop activity, pet gets sadder / dirtier / goes to sleep. Comes back happy on next heartbeat burst. Gives the coming-back-to-your-desk moment emotional weight.
- [ ] **Multi-pet support** (v2+): port the "18 pets × 7 anims" concept from the ESP32 reference. Gate on `preference` key TBD. Could be "each Flipper picks a pet at first pair, stored in furi_hal prefs".

## Out of scope for now

- Flipper Momentum / RogueMaster firmware support — test on those after the core is stable on Unleashed
- Windows testing — no Windows box handy
- Non-Flipper-Zero hardware targets (nRF52, etc.) — the point is the Flipper port
