# ACI instrument authoring guide

An ACI instrument is **one WebAssembly file**. Write it in **C** with `#include "aci.h"`
(bundled at `sdk/aci.h`; no libc, no other includes). `aicontact.py deploy file.c`
compiles, validates and hot-deploys it.

## Rules

- Define these functions (the header already declares them with the right exports):
  - `ACI_META("...json...")` — metadata (defines `aci_meta` + `aci_abi_version`)
  - `int aci_init(float sample_rate, int max_frames)` — return 0. Called once. Store the sample rate.
  - `float* aci_output(int ch)` — return your static output buffer (0 = L, 1 = R; mono: same buffer)
  - `void aci_process(int frames)` — **overwrite** `out[0..frames)` (frames ≤ 256, never more than 4096)
  - `void aci_note_on(int key, float velocity)` — key 0-127, velocity 0..1
  - `void aci_note_off(int key, float velocity)`
  - optional: `aci_set_param(int id, float value)`, `aci_pitch_bend(float -1..1)`,
    `aci_control_change(int cc, float 0..1)`, `aci_channel_pressure(float)`, `aci_all_notes_off()`, `aci_reset()`
- **Everything static**: buffers, voices, delay lines as global arrays. No malloc.
- No imports except logging. Math is provided by aci.h under the usual names:
  `sinf cosf tanf asinf acosf atanf atan2f sinhf coshf tanhf expf exp2f logf log2f log10f powf sqrtf
  fabsf floorf ceilf truncf roundf fmodf fminf fmaxf` (and the double versions), plus `memset`, `memcpy`,
  `memmove`. Do not include `<math.h>` or any other header.
- Parameters: `aci_set_param` receives **plain values** within the declared min..max (not 0..1).
  The host calls it for every param right after `aci_init`. Keep param **ids stable** across versions:
  on hot reload, values are carried over by id.
- The host handles MIDI channel routing, key ranges, transpose, sustain pedal (note-offs are delayed
  while CC64 is down unless `"host_sustain": false`), volume/pan. Don't reimplement those.
- Output level: aim for peaks around **-6 dBFS** with several voices. NaN/Inf output gets muted.
- CPU: the plugin runs wasm in an interpreter (~4-5x slower than native code). `validate` reports
  `cpu_percent` for the test chord measured with the plugin's engine; keep it well under ~30%.
  A hung `aci_process` is stopped after ~20-40 ms.

## Metadata

The plugin builds each instrument's control panel from `params`: a knob per param (`"scale":"log"` for
frequencies/times, `unit` is displayed: `Hz`, `s`, `dB`, `%`), and a drop-down for stepped params with
`labels`. Give params short, musical names and sensible ranges/defaults: that *is* the instrument's UI
unless you also ship a custom one (HTML page or image skin, see `reference/ui.md`).


```json
{"aci":1, "id":"me.bass.acid", "name":"Acid Bass", "version":"1.0.0", "author":"…",
 "description":"…", "tags":["bass"], "outputs":2, "polyphony":1, "host_sustain":true,
 "params":[{"id":0,"name":"Cutoff","min":40,"max":12000,"default":800,"unit":"Hz","scale":"log"},
           {"id":1,"name":"Wave","min":0,"max":1,"default":0,"steps":2,"labels":["Saw","Square"]}]}
```
Inside C the JSON is a string literal: escape quotes as `\"`. Adjacent string literals concatenate.

## Helpers in aci.h

| helper | use |
|---|---|
| `aci_mtof(key)` | MIDI key → Hz (float key allows bends) |
| `aci_db_to_gain(db)` | dB → linear |
| `aci_saw(phase, dt)`, `aci_square(phase, dt)` | band-limited (PolyBLEP) oscillators; phase 0..1, dt = freq/sr |
| `aci_polyblep(t, dt)` | the PolyBLEP correction, for your own band-limited waveforms |
| `aci_noise(&u32_state)` | white noise −1..1 |
| `aci_adsr` + `aci_adsr_set(e, sr, a, d, s, r)`, `aci_adsr_gate(e, on)`, `aci_adsr_next(e)` | envelope; `e.stage == ACI_ADSR_IDLE` when finished |
| `aci_svf` + `aci_svf_tick(f, x, cutoff_hz, q, sr)` → `.lp .bp .hp` | resonant state-variable filter |
| `aci_voice_slot` + `aci_voice_alloc(slots, n, key, &clock)` | polyphonic voice allocation with oldest-note stealing |
| `aci_clamp`, `aci_lerp`, `ACI_PI`, `ACI_TWOPI` | |
| `aci_print("text")`, `aci_print_n(ptr, len)` | debug log → `aicontact.py logs` (rate-limited; never call per sample) |

## Template (polyphonic synth)

```c
#include "aci.h"

ACI_META("{\"aci\":1,\"id\":\"me.template\",\"name\":\"Template\",\"version\":\"1.0.0\",\"outputs\":2,"
         "\"params\":[{\"id\":0,\"name\":\"Cutoff\",\"min\":60,\"max\":16000,\"default\":3000,\"unit\":\"Hz\",\"scale\":\"log\"},"
         "{\"id\":1,\"name\":\"Release\",\"min\":0.01,\"max\":4,\"default\":0.3,\"unit\":\"s\"}]}")

#define VOICES 8
static float outL[ACI_MAX_FRAMES], outR[ACI_MAX_FRAMES];
static float sr = 48000, cutoff = 3000, release = 0.3f, bend = 0;
typedef struct { float key, vel, phase; aci_adsr env; aci_svf filt; } Voice;
static Voice v[VOICES];
static aci_voice_slot slots[VOICES];
static unsigned int clock;

int aci_init(float sample_rate, int max_frames) { sr = sample_rate; return 0; }
float* aci_output(int ch) { return ch == 0 ? outL : outR; }
void aci_set_param(int id, float x) { if (id == 0) cutoff = x; else if (id == 1) release = x; }
void aci_pitch_bend(float x) { bend = x * 2.0f; }   /* ±2 semitones */

void aci_note_on(int key, float vel) {
    int i = aci_voice_alloc(slots, VOICES, key, &clock);
    v[i].key = (float)key; v[i].vel = vel;
    aci_adsr_set(&v[i].env, sr, 0.005f, 0.2f, 0.7f, release);
    aci_adsr_gate(&v[i].env, 1);
}
void aci_note_off(int key, float vel) {
    for (int i = 0; i < VOICES; ++i)
        if (slots[i].active && slots[i].key == key) { aci_adsr_gate(&v[i].env, 0); slots[i].key = -1; }
}
void aci_process(int frames) {
    for (int n = 0; n < frames; ++n) outL[n] = outR[n] = 0;
    for (int i = 0; i < VOICES; ++i) {
        if (!slots[i].active) continue;
        float dt = aci_mtof(v[i].key + bend) / sr;
        for (int n = 0; n < frames; ++n) {
            float s = aci_saw(v[i].phase, dt);
            v[i].phase += dt; if (v[i].phase >= 1) v[i].phase -= 1;
            s = aci_svf_tick(&v[i].filt, s, cutoff, 0.9f, sr).lp * aci_adsr_next(&v[i].env) * v[i].vel * 0.2f;
            outL[n] += s; outR[n] += s;
        }
        if (v[i].env.stage == ACI_ADSR_IDLE) slots[i].active = 0;   /* free finished voices */
    }
}
```

## Patterns

- **Drums / one-shots**: ignore `aci_note_off`, set `"host_sustain": false`, map keys to the GM drum map
  (36 kick, 38 snare, 39 clap, 42 closed hat, 46 open hat, 49 crash) and free a voice when its envelope ends.
- **Monophonic / legato**: keep a small stack of held keys; on note-off return to the previous key.
- **Smoothing**: ramp parameters that cause clicks (`x += (target - x) * 0.001f` per sample).
- **Free voices** when their envelope is idle — the validator reports "does not decay after note-off" otherwise.
- **Stereo**: give `"outputs":2` and fill both buffers; detune/pan voices for width.

## Workflow

1. Write `<name>.c` in the folder the user keeps instruments in; the build `<name>.aci.wasm` lands next to it
   and DAW projects load it from there, so it must stay put.
2. `aicontact.py deploy pad.c --slot pad --channel 1` → fix any build/validation errors it returns.
3. `aicontact.py note C3 E3 G3 --slot pad` to audition in the DAW.
4. Edit the source and run `deploy` again with the **same `--slot`** to hot-reload.
5. `aicontact.py params pad Cutoff=800` for quick tweaks; `aicontact.py logs` if something crashes.
6. `aicontact.py validate pad.c --wav out.wav` renders offline to a WAV file.
