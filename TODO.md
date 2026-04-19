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
- [x] v0.3.1: tokens-delta → feed direct path (fixes feed-always-empty on tool sessions)
- [x] Phase 5h: egg-crack progression within Egg stage (ages 4-9)
- [x] v0.3.3: approvals feed the pet (+50 per OK)
- [x] v0.3.4: cap per-msg play/feed at 15 (happy bar no longer maxes in 5 msgs)
- [x] v0.3.5: retune tokens→feed rate for real session pace
- [x] GitHub release tagged, repo public, .fap attached as asset

## Now

*(nothing actively in progress)*

## Next up — low-hanging polish

- [ ] **README rewrite** — current README has some speculative troubleshooting from the debugging thrash. Replace with the validated flow + screenshots of the Flipper UI in each state (incl. all 5 life stages)
- [ ] **Richer `summarize_msg` ladder** — observe real desktop msg strings during a session (FURI_LOG on unlabeled msgs), extend pattern list so age-bump rate reflects only meaningful interactions, not every streaming status the desktop sends

## Animated mascot — historical Phase 5d plan (shipped)

### Phase 5d plan — animated frames

Goal: replace each static `I_mascot_<state>_64x64` with a 2-6 frame
animation that plays while that state is active. Flipper's
`IconAnimation` API handles the timer + redraws; our draw callback
just picks which animation is current.

**Step 1 — asset pipeline conversion** (~30 min, mostly repo wrangling)

Directory layout flips from flat single PNGs to one subdir per
animation, each containing ordered frames + a frame-rate text file:

```
claude_buddy/assets/
├── mascot_idle_64x64/
│   ├── frame_00.png
│   ├── frame_01.png
│   └── frame_rate         (contains just "2")
├── mascot_sleep_64x64/
│   ├── frame_00.png
│   ├── frame_01.png
│   ├── frame_02.png
│   ├── frame_03.png
│   └── frame_rate         (contains "3")
├── mascot_busy_64x64/
│   ├── frame_00.png ... frame_03.png
│   └── frame_rate         (contains "6")
└── mascot_attention_64x64/
    ├── frame_00.png
    ├── frame_01.png
    └── frame_rate         (contains "3")
```

Symbols become `A_mascot_<state>_64x64` (the `I_` prefix is for
statics, `A_` for animated — ref: `reference/scripts/assets.py:138` vs
`:179`). The Icon struct changes from `I_` to `A_`, but the
`canvas_draw_icon_animation` call signature only takes an
`IconAnimation*` not an `Icon*`, so we need to alloc + tie first.

**Step 2 — generate frames** (~1–2 hrs, mostly art)

Motion notes per state:

  - **idle (2f @ 2 FPS)**. Subtle breathing: frame 0 is current base
    pose, frame 1 is the same sprite shifted down 1 px (body-rows
    31-55 instead of 30-54). Creates a gentle up-down bob.

  - **sleep (4f @ 2 FPS)**. Zzz trail drifts upward: each frame moves
    all three Z glyphs one row up, so they cascade out the top and
    new ones spawn at the bottom. Body unchanged.

  - **busy (4f @ 6 FPS)**. Thinking dots cycle: each frame highlights
    one of the three dots larger than the others (0:big small small,
    1:small big small, 2:small small big, 3:blank). Pupils also look
    around — cycle pupil position through L/C/R/L.

  - **attention (2f @ 3 FPS)**. Pulse: frame 0 = base attention
    sprite, frame 1 = same but "!" glyph grown 1 px in each dimension
    and pupils dilated from 1×1 to 2×2. Simulates excited pulsing.

Frame counts kept small because the `heatshrink`-compressed Icon
data is embedded in the `.fap` ELF — each extra 64×64 frame is
~300-500 B. 12 frames total ≈ 5 KB, trivial.

**Step 3 — wiring** (~30 min code)

In `claude_buddy.c`:

```c
typedef struct {
    // existing fields...
    IconAnimation* current_anim;
    PetState current_anim_state;  // which state the anim represents
} ClaudeBuddyApp;
```

Helper to (re-)install the right animation when pet state changes:

```c
static const Icon* anim_for_state(PetState s) {
    switch(s) {
    case PetStateSleep:     return &A_mascot_sleep_64x64;
    case PetStateBusy:      return &A_mascot_busy_64x64;
    case PetStateAttention: return &A_mascot_attention_64x64;
    default:                return &A_mascot_idle_64x64;
    }
}

static void swap_anim(ClaudeBuddyApp* app, PetState s) {
    if(s == app->current_anim_state && app->current_anim) return;
    if(app->current_anim) {
        icon_animation_stop(app->current_anim);
        icon_animation_free(app->current_anim);
    }
    app->current_anim = icon_animation_alloc(anim_for_state(s));
    icon_animation_start(app->current_anim);
    app->current_anim_state = s;
}
```

Call `swap_anim` from the spot in `handle_rx_line` / status callback
where pet state is derived. Replace `canvas_draw_icon(canvas, 0, 0,
pet_sprite(state))` with `canvas_draw_icon_animation(canvas, 0, 0,
app->current_anim)`.

Retire the 250 ms redraw timer — `view_tie_icon_animation(view_port,
anim)` ties the animation's per-frame fires to viewport updates
automatically. (Keep a minimal timer for non-animation fields like
the BT status line refreshing.)

**Step 4 — guardrails**

  - Lock app->mtx during swap_anim (other threads read current_anim
    from the draw callback)
  - Teardown order: stop anim, free anim, then tear down view_port
  - Verify the animation timer (FreeRTOS Timer Service task) plays
    well with our RX drain thread (already 2 KB stack, should be fine)
  - Watch FAP binary size — each compressed frame is small but a
    misconfigured `frame_rate` (e.g. 60) could blow through the CPU
    budget with heatshrink decode

**Step 5 — on-device validate**, state by state (5 min)

- sleep → Zzz drifting upward
- idle → gentle bob
- busy → dots cycling, pupils tracking
- attention → "!" pulsing, pupils dilating

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
