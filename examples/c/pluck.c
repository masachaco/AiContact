/* Pluck: Karplus-Strong plucked string (8 voices). Shows per-voice delay lines
 * allocated statically. */
#include "aci.h"

ACI_META("{\"aci\":1,\"id\":\"aicontact.example.pluck\",\"name\":\"Pluck\",\"version\":\"1.0.0\","
         "\"author\":\"AI Contact\",\"tags\":[\"keys\",\"pluck\"],\"outputs\":2,\"params\":["
         "{\"id\":0,\"name\":\"Damping\",\"min\":0,\"max\":1,\"default\":0.35},"
         "{\"id\":1,\"name\":\"Brightness\",\"min\":0,\"max\":1,\"default\":0.7},"
         "{\"id\":2,\"name\":\"Width\",\"min\":0,\"max\":1,\"default\":0.5}]}")

#define VOICES 8
#define MAXLEN 4096   /* lowest note ~ 12 Hz at 48 kHz */

static float outL[ACI_MAX_FRAMES], outR[ACI_MAX_FRAMES];
static float sr = 48000.0f, damping = 0.35f, bright = 0.7f, width = 0.5f;
static unsigned int seed = 777;
typedef struct { float buf[MAXLEN]; int len, pos; float frac, last, gain, pan, release; } String;
static String str[VOICES];
static aci_voice_slot slots[VOICES];
static unsigned int clock;

int aci_init(float sample_rate, int max_frames) { sr = sample_rate; return 0; }
float* aci_output(int ch) { return ch == 0 ? outL : outR; }
void aci_set_param(int id, float v) { if (id == 0) damping = v; else if (id == 1) bright = v; else if (id == 2) width = v; }

void aci_note_on(int key, float vel) {
    int i = aci_voice_alloc(slots, VOICES, key, &clock);
    String* s = &str[i];
    float period = sr / aci_mtof((float)key);
    s->len = (int)period; if (s->len >= MAXLEN) s->len = MAXLEN - 1; if (s->len < 2) s->len = 2;
    s->pos = 0; s->last = 0; s->gain = vel; s->release = 1.0f;
    s->pan = ((key % 12) / 11.0f - 0.5f) * width;
    float lp = 0;  /* excitation: filtered noise burst */
    for (int n = 0; n < s->len; ++n) { lp += (aci_noise(&seed) - lp) * (0.2f + 0.8f * bright); s->buf[n] = lp * vel; }
}

void aci_note_off(int key, float vel) {
    for (int i = 0; i < VOICES; ++i) if (slots[i].active && slots[i].key == key) { str[i].release = 0.9f;  /* per pass through the delay line */ slots[i].key = -1; }
}

void aci_process(int frames) {
    for (int n = 0; n < frames; ++n) outL[n] = outR[n] = 0;
    float fb = 0.999f - damping * 0.02f;
    for (int i = 0; i < VOICES; ++i) {
        if (!slots[i].active) continue;
        String* s = &str[i];
        float peak = 0;
        for (int n = 0; n < frames; ++n) {
            int nxt = s->pos + 1 == s->len ? 0 : s->pos + 1;
            float y = 0.5f * (s->buf[s->pos] + s->buf[nxt]) * fb;
            s->buf[s->pos] = y * s->release;
            s->pos = nxt;
            outL[n] += y * (0.5f - s->pan) * 0.8f;
            outR[n] += y * (0.5f + s->pan) * 0.8f;
            if (fabsf(y) > peak) peak = fabsf(y);
        }
        if (peak < 1e-5f) slots[i].active = 0;
    }
}
