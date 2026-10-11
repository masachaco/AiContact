/* The smallest useful ACI instrument: a polyphonic sine with a simple envelope. */
#include "aci.h"

ACI_META("{\"aci\":1,\"id\":\"aicontact.example.sine\",\"name\":\"Sine\",\"version\":\"1.0.0\","
         "\"outputs\":1,\"params\":[{\"id\":0,\"name\":\"Release\",\"min\":0.01,\"max\":4,\"default\":0.3,\"unit\":\"s\"}]}")

#define VOICES 8
static float out[ACI_MAX_FRAMES];
static float sr = 48000.0f, release = 0.3f;
static struct { float phase, inc, vel; aci_adsr env; } v[VOICES];
static aci_voice_slot slots[VOICES];
static unsigned int clock;

int aci_init(float sample_rate, int max_frames) { sr = sample_rate; return 0; }
float* aci_output(int ch) { return out; }
void aci_set_param(int id, float value) { if (id == 0) release = value; }

void aci_note_on(int key, float vel) {
    int i = aci_voice_alloc(slots, VOICES, key, &clock);
    v[i].inc = aci_mtof((float)key) / sr;
    v[i].vel = vel;
    aci_adsr_set(&v[i].env, sr, 0.005f, 0.1f, 0.7f, release);
    aci_adsr_gate(&v[i].env, 1);
}

void aci_note_off(int key, float vel) {
    for (int i = 0; i < VOICES; ++i)
        if (slots[i].active && slots[i].key == key) { aci_adsr_gate(&v[i].env, 0); slots[i].key = -1; }
}

void aci_process(int frames) {
    for (int n = 0; n < frames; ++n) out[n] = 0.0f;
    for (int i = 0; i < VOICES; ++i) {
        if (!slots[i].active) continue;
        for (int n = 0; n < frames; ++n) {
            out[n] += sinf(ACI_TWOPI * v[i].phase) * aci_adsr_next(&v[i].env) * v[i].vel * 0.25f;
            v[i].phase += v[i].inc;
            if (v[i].phase >= 1.0f) v[i].phase -= 1.0f;
        }
        if (v[i].env.stage == ACI_ADSR_IDLE) slots[i].active = 0;
    }
}
