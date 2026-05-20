# Display Requirements — BatteryMonitorV2

## Purpose

Locks the requirements for the firmware's display layer: visible
screens, what each contains, navigation model, and cross-cutting
behaviour. The screen-router / display-controller architecture is
designed against these requirements as a follow-on (a contour preview
is included at the bottom, but the concrete typedefs and
file layout are a separate exercise).

Drafted as the resolution for PR #6 review comment #3 ("how does
`display_render()` scale to support a menu or alternative views?")
to avoid designing the plumbing before knowing what it has to carry.

---

## Hardware constraints

| Item | Detail |
|---|---|
| Panel | 128 × 32 monochrome OLED, SSD1306-driven, I2C2 at 0x3C |
| Refresh | currently 5 Hz (200 ms/frame); fine for live data, low CPU |
| Input | **2 front-panel buttons** (the front-panel pad supports more, only 2 wired). Pulldown, active-high. No encoder, no touch. Short-press vs long-press distinction carries the navigation. |
| Heartbeat | bottom-right corner heart icon, synced 1 Hz with `MCU_LED` via `direct_io.led_state()`. Persistent across all views. |
| FLASH headroom | currently ~22 KB free (Release at 65.5 %) — plenty for additional views + a menu state machine. |

---

## Data sources

### Live now
- **INA238** (`monitor_snapshot_t`): `vbus_mv`, `current_ma`,
  `vshunt_uv`, `tdie_mc`
- **direct_io**: `k1_on`, `bleed_on`, `fan_duty_pct`, `led_state()`

### Planned
- **Charge state machine** (not yet implemented): charge-state enum
  (Charging / Trickle / No Batt / Test / Fault / …), `charge_time_s`,
  derived metrics (energy delivered, target V/I, fault codes).
- **PN532 NFC** (not yet implemented): battery serial number;
  possibly tag-stored capacity / cycle count / manufacturer.
- **RTC** (TBD): wall-clock time? ambient temperature?

---

## Screens

| ID | Name | Status | Purpose |
|---|---|---|---|
| `MAIN` | Default operational | implemented as V1 | Major operational mode label + battery telem (V or A) + charge timer / S/N + mode indicators (F / K / B / heart). Default boot screen and the most common view. |
| `ALT_THERMAL` | Thermal telem | design now | `tdie_mc` from INA238; room for ambient temperature when an RTC / external sensor lands. Maybe a coarse trend bar. |
| `ALT_COOLING` | Cooling / fan telem | design now | `fan_duty_pct`, plus fan RPM when a tachometer lands. |
| `ALT_BATTERY` | Battery info (NFC) | design now | S/N + tag-stored fields (capacity, cycle count, etc.) once PN532 lands. Stub layout until then. |
| `MENU` | Settings / params | design now | Top-level menu. Items: **Force test**, **Reset**, **Firmware update**, **About**, **Status**. Sub-menus / confirm prompts as needed per item. |
| `IDLE` | Idle screensaver | optional, last priority | Simple graphics played when system is "fully idle" (no battery + no button activity for N seconds). Saves the panel from prolonged static content. |
| `DEBUG` | Free-form debug output | deferred — design shape now, implement later | Direct output target for ad-hoc debug text from any module (printf-style scrolling, like a tiny in-panel CDC tee). Skeleton only in v1; real implementation later. |

### Operational-mode → display-label mapping

The internal charging state machine and the display string for that
state are **decoupled**. The display layer carries a small lookup so
internal names stay technically precise and display names stay
user-facing.

| Internal state | MAIN label | Notes |
|---|---|---|
| `NO_BATT`  | `No Batt` | most-common when no battery plugged in |
| `CHARGING` | `Charging` | bulk / absorption |
| `TRICKLE`  | **`Charged`** | user-visible signal that the battery is full; we stay in trickle to maintain it |
| `TEST`     | `Test` | self-test or manual force-test mode |
| `FAULT`    | `Fault` | label visible while the dedicated FAULT screen takes over (see Faults below) |

### Charge indicator

An estimated charge percentage lives in the **MAIN tray**, fitting in
alongside or rotating with the timer / S/N slot if there's room. The
caller passes a `uint8_t` percentage to the render layer; the
renderer decides best fit. The charge controller decides how to
estimate the percentage — display has no chemistry assumptions.

### Design principle — display stays agnostic about app logic

A future "battery-health warning" (e.g. degradation suspected from
trickle-mode final voltage after a load test) is the controller's
call, not the display's. The render layer just consumes a flag /
status code the controller publishes — no battery-chemistry
assumptions, no thresholds baked into render paths. Same principle
for charge %, estimated time remaining, fault classifications, etc.

---

## Navigation (2 buttons, short / long press)

Two buttons (call them **NEXT** and **SELECT**), with the NEXT
button additionally doubling for **BACK** on a long-press. Consistent
semantics everywhere — long-press always backs out of context.

| Context | NEXT short | NEXT long | SELECT short |
|---|---|---|---|
| `MAIN` / `ALT_*` | cycle to next view (`MAIN → THERMAL → COOLING → BATTERY → MAIN …`) | reset cycle back to MAIN | enter `MENU` |
| `MENU` | cursor → next item | exit MENU back to MAIN | execute / enter sub-menu |
| sub-menu / param-edit | cursor → next item / increment value | back to parent menu | execute / commit / enter |
| `IDLE` | wake to MAIN | wake to MAIN | wake to MAIN |

Long-press detection: rising-edge → start timer → if still pressed at
~750 ms, fire long-press; otherwise short-press on release. Standard
debounce on both edges.

Button input → action mapping lives in a display-controller module —
the render layer stays pure, no event handling. Implementation
details deferred to the architecture plan.

---

## Cross-cutting

### Activity / power (TBD)
- Refresh cadence: keep 5 Hz baseline, or slow down when nothing
  changes (event-driven render)?
- Display sleep: auto-off the OLED after N minutes of inactivity to
  save the panel?
- Wake: any button press? state change (charge starting / ending)?

### Fault behaviour
- **Dedicated FAULT screen overrides current view.** On fault the
  render layer switches to FAULT regardless of current view. Fault
  details (which interlock fired, time, etc.) take the whole screen.
  Long-press NEXT acknowledges / returns to MAIN once the fault is
  cleared.
- One fault at a time (newest wins) — stacking deferred unless
  experience shows it's needed.

### State persistence
- Display returns to MAIN on boot. No "last view" persistence.

### Internationalisation
- ASCII only. The vendored fonts (`bm_font_18b` / `15b` / `5x7`)
  carry the printable-ASCII subset (32–126). No non-Latin glyphs.

---

## Decisions locked

- **Charge indicator location**: MAIN tray (alongside / rotating
  with timer / S/N if there's room). Caller passes `uint8_t`
  percentage; renderer fits it.
- **`Reset` menu item**: **full MCU reboot** via `NVIC_SystemReset()`.
  One thing, no surprises. Counter clears happen as a side effect of
  fresh boot.
- **Destructive menu actions** (Force test, Reset, Firmware update):
  **confirm sub-menu** (Yes / No). Uniform rule across all
  destructive items — avoids stray-press surprises.
- **Faults**: **dedicated FAULT screen overrides current view**
  (details above).

## Still TBD (non-blocking — implementer can decide)

- **Idle screensaver content** — bouncing dot, simple animation,
  brand mark. Cosmetic; pick during implementation.
- **`Firmware update` entry mechanism** — most likely
  `NVIC_SystemReset()` with a magic flag in backup-domain RAM so the
  bootloader picks up DFU mode. Standard pattern; implementation
  detail.

---

## Architecture shape implied (preview — full design is next)

These requirements pull naturally toward four small layers. **This
section is a contour, not the design** — the typedefs, function
signatures, and file layout get fleshed out in a follow-on plan.

1. **Render layer** (`display_render.c` + a per-view file or
   per-view function in the same file). Pure functions, one per view
   (`MAIN`, `ALT_THERMAL`, `ALT_COOLING`, `ALT_BATTERY`, `MENU`,
   `IDLE`, `FAULT`, `DEBUG`-stub). Each takes the minimum data it
   needs. No event handling, no state — just paint a frame.

2. **Display controller** (new module). Owns the current view ID,
   menu cursor, confirm-prompt state, last-seen fault, refresh
   cadence. Picks which render function to call each frame and what
   to pass it.

3. **Input layer** (new module). Reads the 2 buttons via
   `direct_io`, debounces, classifies as short / long press, posts
   events (`NEXT_SHORT` / `NEXT_LONG` / `SELECT_SHORT`) to the
   display controller via FreeRTOS notification or queue.

4. **State→label map** (tiny, in render layer or controller). Static
   table `(charge_state_t, fault?) → const char *` so internal
   names (`TRICKLE`) stay decoupled from display strings
   (`Charged`).

`display_task.c` (the current placeholder, marked TODO) gets
replaced by / collapses into the display controller. Render layer
stays pure and self-contained — a future SDL desktop sim can drive
any view with synthetic inputs.

---

## Out of scope

- Concrete architecture (typedefs, function signatures, file
  layout). Next planning session — once these reqs are signed off.
- Charge-state machine design (separate concern; display just
  consumes its outputs).
- NFC driver / tag data model (separate concern; display just
  consumes its outputs).
- Button debounce / input layer (its own small module per the
  contour above; design later).
- Battery-health heuristics (app logic in the charge controller,
  not a display concern).
