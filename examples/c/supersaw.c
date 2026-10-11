/* SuperSaw: 16-voice polyphonic subtractive synth.
 * 3 detuned PolyBLEP saws per voice spread in stereo -> resonant SVF low-pass
 * with its own envelope. A good starting point for pads, leads and basses. */
#include "aci.h"

ACI_META("{\"aci\":1,\"id\":\"aicontact.example.supersaw\",\"name\":\"SuperSaw\",\"version\":\"1.0.0\","
         "\"author\":\"AI Contact\",\"description\":\"Detuned saw poly synth with resonant filter\","
         "\"tags\":[\"synth\",\"pad\",\"lead\"],\"outputs\":2,\"polyphony\":16,\"params\":["
         "{\"id\":0,\"name\":\"Cutoff\",\"min\":40,\"max\":18000,\"default\":2500,\"unit\":\"Hz\",\"scale\":\"log\"},"
         "{\"id\":1,\"name\":\"Resonance\",\"min\":0.5,\"max\":8,\"default\":1.2},"
         "{\"id\":2,\"name\":\"Env Amount\",\"min\":0,\"max\":1,\"default\":0.4},"
         "{\"id\":3,\"name\":\"Detune\",\"min\":0,\"max\":1,\"default\":0.3},"
         "{\"id\":4,\"name\":\"Attack\",\"min\":0.001,\"max\":4,\"default\":0.01,\"unit\":\"s\",\"scale\":\"log\"},"
         "{\"id\":5,\"name\":\"Release\",\"min\":0.01,\"max\":6,\"default\":0.4,\"unit\":\"s\",\"scale\":\"log\"},"
         "{\"id\":6,\"name\":\"Volume\",\"min\":-40,\"max\":6,\"default\":-6,\"unit\":\"dB\"}]}")

#define VOICES 16
#define OSC 3

static float outL[ACI_MAX_FRAMES], outR[ACI_MAX_FRAMES];
static float sr = 48000.0f;
static float cutoff = 2500, reso = 1.2f, envAmt = 0.4f, detune = 0.3f, attack = 0.01f, release = 0.4f, volume = 0.5f;
static float bend = 0.0f;  /* semitones */

typedef struct {
    float key, vel, phase[OSC];
    aci_adsr amp, filt;
    aci_svf svfL, svfR;
} Voice;
static Voice voices[VOICES];
static aci_voice_slot slots[VOICES];
static unsigned int clock;

static const float spread[OSC] = {-1.0f, 0.0f, 1.0f};
static const float panL[OSC] = {1.0f, 0.7f, 0.3f};

int aci_init(float sample_rate, int max_frames) { sr = sample_rate; return 0; }
float* aci_output(int ch) { return ch == 0 ? outL : outR; }

void aci_set_param(int id, float v) {
    switch (id) {
    case 0: cutoff = v; break;
    case 1: reso = v; break;
    case 2: envAmt = v; break;
    case 3: detune = v; break;
    case 4: attack = v; break;
    case 5: release = v; break;
    case 6: volume = aci_db_to_gain(v); break;
    }
}

void aci_pitch_bend(float v) { bend = v * 2.0f; }

void aci_note_on(int key, float vel) {
    int i = aci_voice_alloc(slots, VOICES, key, &clock);
    Voice* v = &voices[i];
    v->key = (float)key;
    v->vel = vel;
    aci_adsr_set(&v->amp, sr, attack, 0.3f, 0.8f, release);
    aci_adsr_set(&v->filt, sr, attack * 0.5f + 0.002f, 0.6f, 0.2f, release);
    aci_adsr_gate(&v->amp, 1);
    aci_adsr_gate(&v->filt, 1);
}

void aci_note_off(int key, float vel) {
    for (int i = 0; i < VOICES; ++i)
        if (slots[i].active && slots[i].key == key) {
            aci_adsr_gate(&voices[i].amp, 0);
            aci_adsr_gate(&voices[i].filt, 0);
            slots[i].key = -1;  /* a re-pressed key gets a fresh voice */
        }
}

void aci_all_notes_off(void) { for (int i = 0; i < VOICES; ++i) aci_note_off(slots[i].key, 0); }

void aci_process(int frames) {
    for (int n = 0; n < frames; ++n) outL[n] = outR[n] = 0.0f;
    for (int i = 0; i < VOICES; ++i) {
        if (!slots[i].active) continue;
        Voice* v = &voices[i];
        float inc[OSC];
        for (int o = 0; o < OSC; ++o)
            inc[o] = aci_mtof(v->key + bend + spread[o] * detune * 0.3f) / sr;
        for (int n = 0; n < frames; ++n) {
            float l = 0.0f, r = 0.0f;
            for (int o = 0; o < OSC; ++o) {
                float s = aci_saw(v->phase[o], inc[o]);
                v->phase[o] += inc[o];
                if (v->phase[o] >= 1.0f) v->phase[o] -= 1.0f;
                l += s * panL[o];
                r += s * (1.3f - panL[o]);
            }
            float fenv = aci_adsr_next(&v->filt);
            float fc = cutoff * powf(2.0f, envAmt * 5.0f * fenv);  /* up to +5 octaves */
            float a = aci_adsr_next(&v->amp) * v->vel * 0.15f;
            outL[n] += aci_svf_tick(&v->svfL, l, fc, reso, sr).lp * a;
            outR[n] += aci_svf_tick(&v->svfR, r, fc, reso, sr).lp * a;
        }
        if (v->amp.stage == ACI_ADSR_IDLE) slots[i].active = 0;
    }
    for (int n = 0; n < frames; ++n) { outL[n] *= volume; outR[n] *= volume; }
}
