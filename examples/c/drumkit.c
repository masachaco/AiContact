/* DrumKit: synthesized drums on the General MIDI drum map.
 * 36 kick, 38 snare, 39 clap, 42 closed hat, 46 open hat, 45/47/50 toms, 49 crash.
 * One-shot voices: note-off is ignored. */
#include "aci.h"

ACI_META("{\"aci\":1,\"id\":\"aicontact.example.drumkit\",\"name\":\"DrumKit\",\"version\":\"1.0.0\","
         "\"author\":\"AI Contact\",\"description\":\"Synthesized GM-mapped drum kit\",\"tags\":[\"drums\"],"
         "\"outputs\":2,\"host_sustain\":false,\"params\":["
         "{\"id\":0,\"name\":\"Kick Tune\",\"min\":30,\"max\":90,\"default\":50,\"unit\":\"Hz\"},"
         "{\"id\":1,\"name\":\"Kick Decay\",\"min\":0.1,\"max\":1.5,\"default\":0.45,\"unit\":\"s\"},"
         "{\"id\":2,\"name\":\"Snare Tone\",\"min\":0,\"max\":1,\"default\":0.4},"
         "{\"id\":3,\"name\":\"Hat Decay\",\"min\":0.02,\"max\":0.6,\"default\":0.06,\"unit\":\"s\"},"
         "{\"id\":4,\"name\":\"Volume\",\"min\":-40,\"max\":6,\"default\":-10,\"unit\":\"dB\"}]}")

enum { KICK, SNARE, CLAP, HAT, OHAT, TOM, CRASH };
#define VOICES 12

static float outL[ACI_MAX_FRAMES], outR[ACI_MAX_FRAMES];
static float sr = 48000.0f, kickTune = 50, kickDecay = 0.45f, snareTone = 0.4f, hatDecay = 0.06f, volume = 0.32f;
static unsigned int seed = 12345;

typedef struct { int type, active; float t, vel, phase, freq, pan; aci_svf f; } Drum;
static Drum d[VOICES];
static int next;

int aci_init(float sample_rate, int max_frames) { sr = sample_rate; return 0; }
float* aci_output(int ch) { return ch == 0 ? outL : outR; }

void aci_set_param(int id, float v) {
    if (id == 0) kickTune = v; else if (id == 1) kickDecay = v; else if (id == 2) snareTone = v;
    else if (id == 3) hatDecay = v; else if (id == 4) volume = aci_db_to_gain(v);
}

void aci_note_on(int key, float vel) {
    int type; float freq = 0, pan = 0;
    switch (key) {
    case 35: case 36: type = KICK; break;
    case 37: case 38: case 40: type = SNARE; break;
    case 39: type = CLAP; pan = 0.1f; break;
    case 42: case 44: type = HAT; pan = 0.3f; break;
    case 46: type = OHAT; pan = 0.3f; break;
    case 41: case 43: case 45: type = TOM; freq = 90; pan = -0.3f; break;
    case 47: case 48: type = TOM; freq = 130; break;
    case 50: type = TOM; freq = 180; pan = 0.3f; break;
    case 49: case 57: type = CRASH; pan = -0.2f; break;
    default: return;
    }
    if (type == HAT) for (int i = 0; i < VOICES; ++i) if (d[i].type == OHAT) d[i].active = 0;  /* choke */
    Drum* v = &d[next]; next = (next + 1) % VOICES;
    v->type = type; v->active = 1; v->t = 0; v->vel = vel; v->phase = 0; v->freq = freq; v->pan = pan;
    v->f.ic1 = v->f.ic2 = 0;
}

void aci_note_off(int key, float vel) {}

static float voice_tick(Drum* v) {
    float t = v->t, s = 0, dt = 1.0f / sr;
    switch (v->type) {
    case KICK: {
        float f = kickTune + 180.0f * expf(-t * 35.0f);
        v->phase += f * dt; if (v->phase >= 1) v->phase -= 1;
        s = sinf(ACI_TWOPI * v->phase) * expf(-t / kickDecay * 3.0f);
        s += aci_noise(&seed) * expf(-t * 400.0f) * 0.3f;  /* beater click */
        if (t > kickDecay * 3) v->active = 0;
        break;
    }
    case SNARE: {
        v->phase += (180.0f + 40.0f * snareTone) * dt; if (v->phase >= 1) v->phase -= 1;
        float body = sinf(ACI_TWOPI * v->phase) * expf(-t * 25.0f);
        float noise = aci_svf_tick(&v->f, aci_noise(&seed), 2500.0f + 4000.0f * snareTone, 0.8f, sr).hp * expf(-t * 14.0f);
        s = body * 0.6f + noise * 0.8f;
        if (t > 0.5f) v->active = 0;
        break;
    }
    case CLAP: {
        float env = 0; for (int k = 0; k < 3; ++k) { float tk = t - k * 0.011f; if (tk >= 0) env += expf(-tk * 120.0f); }
        env += (t > 0.022f) ? expf(-(t - 0.022f) * 12.0f) * 0.6f : 0;
        s = aci_svf_tick(&v->f, aci_noise(&seed), 1400.0f, 2.0f, sr).bp * env * 1.6f;
        if (t > 0.6f) v->active = 0;
        break;
    }
    case HAT: case OHAT: case CRASH: {
        float decay = v->type == HAT ? hatDecay : v->type == OHAT ? hatDecay * 6.0f : 1.4f;
        s = aci_svf_tick(&v->f, aci_noise(&seed), v->type == CRASH ? 5000.0f : 8000.0f, 0.7f, sr).hp
            * expf(-t / decay) * (v->type == CRASH ? 0.5f : 0.45f);
        if (t > decay * 8) v->active = 0;
        break;
    }
    case TOM: {
        float f = v->freq * (1.0f + 0.5f * expf(-t * 20.0f));
        v->phase += f * dt; if (v->phase >= 1) v->phase -= 1;
        s = sinf(ACI_TWOPI * v->phase) * expf(-t * 7.0f);
        if (t > 0.9f) v->active = 0;
        break;
    }
    }
    v->t += dt;
    return s * v->vel;
}

void aci_process(int frames) {
    for (int n = 0; n < frames; ++n) {
        float l = 0, r = 0;
        for (int i = 0; i < VOICES; ++i) {
            if (!d[i].active) continue;
            float s = voice_tick(&d[i]);
            l += s * (1.0f - d[i].pan);
            r += s * (1.0f + d[i].pan);
        }
        outL[n] = aci_clamp(l * volume, -1.5f, 1.5f);
        outR[n] = aci_clamp(r * volume, -1.5f, 1.5f);
    }
}
