/*
 * aci.h — AI Contact Instrument (ACI) ABI v1 authoring header for C / C++.
 * SPDX-License-Identifier: MIT-0
 * (no attribution needed: this code is compiled into the instruments you build with it)
 *
 * An ACI instrument is a single WebAssembly binary (.wasm) that exports the
 * functions declared below. See docs/ACI-SPEC.md for the normative spec.
 *
 * Build (no libc / sysroot needed):
 *   clang --target=wasm32 -O2 -nostdlib -fno-builtin -Wl,--no-entry \
 *         -Wl,--allow-undefined -I sdk/c -o my_synth.wasm my_synth.c
 *
 * Do not include <math.h>/<string.h>: this header provides the common libc
 * math and memory functions (sinf, powf, tanhf, memcpy, ...) built into the
 * module, because an ACI module may import nothing but env.aci_log.
 *
 * Minimal instrument:
 *   #include "aci.h"
 *   ACI_META("{\"aci\":1,\"id\":\"me.sine\",\"name\":\"Sine\"}")
 *   static float out[ACI_MAX_FRAMES];
 *   int   aci_init(float sr, int max_frames) { ... return 0; }
 *   float* aci_output(int ch) { return out; }      // mono: same buffer for L/R
 *   void  aci_note_on(int key, float vel)  { ... }
 *   void  aci_note_off(int key, float vel) { ... }
 *   void  aci_process(int frames)          { ... write out[0..frames) ... }
 *
 * aci_abi_version() is defined by ACI_META() for you.
 */
#ifndef ACI_H
#define ACI_H

#ifdef __cplusplus
extern "C" {
#endif

#define ACI_ABI_VERSION 1
/* Hosts never pass max_frames larger than this, so static buffers of this
 * size are always sufficient. */
#define ACI_MAX_FRAMES 4096

#define ACI_EXPORT(name) __attribute__((export_name(#name), visibility("default")))
#define ACI_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

/* ------------------------------------------------------------------------- */
/* Exports (define the ones you need; required ones are marked)               */
/* ------------------------------------------------------------------------- */

ACI_EXPORT(aci_abi_version)     int    aci_abi_version(void);               /* required (ACI_META defines it) */
ACI_EXPORT(aci_meta)            const char* aci_meta(void);                 /* recommended: JSON metadata */
ACI_EXPORT(aci_init)            int    aci_init(float sample_rate, int max_frames); /* required, 0 = ok */
ACI_EXPORT(aci_output)          float* aci_output(int channel);             /* required: 0 = L, 1 = R */
ACI_EXPORT(aci_process)         void   aci_process(int frames);             /* required */
ACI_EXPORT(aci_note_on)         void   aci_note_on(int key, float velocity);  /* required, velocity 0..1 */
ACI_EXPORT(aci_note_off)        void   aci_note_off(int key, float velocity); /* required */
ACI_EXPORT(aci_set_param)       void   aci_set_param(int id, float value);  /* optional, plain value */
ACI_EXPORT(aci_pitch_bend)      void   aci_pitch_bend(float value);         /* optional, -1..1 */
ACI_EXPORT(aci_control_change)  void   aci_control_change(int cc, float value); /* optional, 0..1 */
ACI_EXPORT(aci_channel_pressure) void  aci_channel_pressure(float value);   /* optional, 0..1 */
ACI_EXPORT(aci_all_notes_off)   void   aci_all_notes_off(void);             /* optional */
ACI_EXPORT(aci_reset)           void   aci_reset(void);                     /* optional */

/* Embeds the metadata JSON and defines aci_meta() + aci_abi_version(). */
#define ACI_META(json) \
    const char* aci_meta(void) { return json; } \
    int aci_abi_version(void) { return ACI_ABI_VERSION; }

/* ------------------------------------------------------------------------- */
/* Host import: the ONLY import an ACI module may use (optional).            */
/* ------------------------------------------------------------------------- */

ACI_IMPORT(aci_log) void aci_print_n(const char* msg, int len);

/* ------------------------------------------------------------------------- */
/* Self-contained libc subset. ACI modules must not import anything else, so */
/* math and memory functions are compiled into the module from here.         */
/* Accuracy is ~1e-7 relative (float), plenty for audio.                     */
/* ------------------------------------------------------------------------- */

typedef __SIZE_TYPE__ aci_size_t;

/* The compiler may emit calls to these for struct copies / loops. */
__attribute__((weak)) void* memset(void* d, int c, aci_size_t n) {
    unsigned char* p = (unsigned char*)d; while (n--) *p++ = (unsigned char)c; return d;
}
__attribute__((weak)) void* memcpy(void* d, const void* s, aci_size_t n) {
    unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s;
    while (n--) *p++ = *q++; return d;
}
__attribute__((weak)) void* memmove(void* d, const void* s, aci_size_t n) {
    unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s;
    if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

static inline double aci__round(double x) { return __builtin_nearbyint(x); }
static inline double aci__ldexp(double x, int e) {   /* x * 2^e, e clamped */
    if (e > 1023) e = 1023; if (e < -1022) return 0.0;
    union { double d; unsigned long long u; } u = { 0 };
    u.u = (unsigned long long)(e + 1023) << 52;
    return x * u.d;
}
static inline double aci__sin_k(double r) { double r2 = r * r;
    return r * (1.0 + r2 * (-1.0/6 + r2 * (1.0/120 + r2 * (-1.0/5040 + r2 * (1.0/362880 + r2 * (-1.0/39916800)))))); }
static inline double aci__cos_k(double r) { double r2 = r * r;
    return 1.0 + r2 * (-0.5 + r2 * (1.0/24 + r2 * (-1.0/720 + r2 * (1.0/40320 + r2 * (-1.0/3628800 + r2 * (1.0/479001600)))))); }
static inline double aci_sin(double x) {
    double q = aci__round(x * 0.63661977236758134), r = x - q * 1.5707963267948966;
    int k = (int)((long long)q & 3);
    return k == 0 ? aci__sin_k(r) : k == 1 ? aci__cos_k(r) : k == 2 ? -aci__sin_k(r) : -aci__cos_k(r);
}
static inline double aci_cos(double x) {
    double q = aci__round(x * 0.63661977236758134), r = x - q * 1.5707963267948966;
    int k = (int)((long long)q & 3);
    return k == 0 ? aci__cos_k(r) : k == 1 ? -aci__sin_k(r) : k == 2 ? -aci__cos_k(r) : aci__sin_k(r);
}
static inline double aci_tan(double x) { return aci_sin(x) / aci_cos(x); }
static inline double aci_exp(double x) {
    if (x != x) return x;
    if (x > 709.0) return 1e308 * 10.0;
    if (x < -745.0) return 0.0;
    double t = x * 1.4426950408889634, n = aci__round(t), f = (t - n) * 0.6931471805599453;
    double p = 1.0 + f * (1.0 + f * (0.5 + f * (1.0/6 + f * (1.0/24 + f * (1.0/120 + f * (1.0/720 + f * (1.0/5040 + f * (1.0/40320))))))));
    return aci__ldexp(p, (int)n);
}
static inline double aci_exp2(double x) { return aci_exp(x * 0.6931471805599453); }
static inline double aci_log(double x) {
    if (x != x || x < 0.0) return 0.0 / 0.0;
    if (x == 0.0) return -1e308 * 10.0;
    union { double d; unsigned long long u; } u = { x };
    int e = (int)((u.u >> 52) & 0x7ff) - 1023;
    if (e == -1023) { u.d = x * 4503599627370496.0; e = (int)((u.u >> 52) & 0x7ff) - 1023 - 52; } /* subnormal */
    u.u = (u.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;      /* m in [1,2) */
    double m = u.d; if (m > 1.4142135623730951) { m *= 0.5; ++e; }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    double l = 2.0 * s * (1.0 + s2 * (1.0/3 + s2 * (1.0/5 + s2 * (1.0/7 + s2 * (1.0/9 + s2 * (1.0/11 + s2 * (1.0/13)))))));
    return l + e * 0.6931471805599453;
}
static inline double aci_log2(double x) { return aci_log(x) * 1.4426950408889634; }
static inline double aci_log10(double x) { return aci_log(x) * 0.4342944819032518; }
static inline double aci_pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x > 0.0) return aci_exp(y * aci_log(x));
    if (x == 0.0) return y > 0.0 ? 0.0 : 1e308 * 10.0;
    double yi = __builtin_trunc(y);
    if (yi != y) return 0.0 / 0.0;
    double r = aci_exp(y * aci_log(-x));
    return ((long long)yi & 1) ? -r : r;
}
static inline double aci_tanh(double x) {
    if (x > 20.0) return 1.0; if (x < -20.0) return -1.0;
    double e = aci_exp(2.0 * x); return (e - 1.0) / (e + 1.0);
}
static inline double aci_sinh(double x) { double e = aci_exp(x); return 0.5 * (e - 1.0 / e); }
static inline double aci_cosh(double x) { double e = aci_exp(x); return 0.5 * (e + 1.0 / e); }
static inline double aci_atan(double x) {
    /* two half-angle reductions bring |x| <= tan(pi/16), then a short series */
    x = x / (1.0 + __builtin_sqrt(1.0 + x * x));
    x = x / (1.0 + __builtin_sqrt(1.0 + x * x));
    double x2 = x * x;
    return 4.0 * x * (1.0 + x2 * (-1.0/3 + x2 * (1.0/5 + x2 * (-1.0/7 + x2 * (1.0/9 + x2 * (-1.0/11 + x2 * (1.0/13)))))));
}
static inline double aci_atan2(double y, double x) {
    if (x > 0.0) return aci_atan(y / x);
    if (x < 0.0) return aci_atan(y / x) + (y >= 0.0 ? 3.141592653589793 : -3.141592653589793);
    return y > 0.0 ? 1.5707963267948966 : (y < 0.0 ? -1.5707963267948966 : 0.0);
}
static inline double aci_asin(double x) { return aci_atan2(x, __builtin_sqrt(1.0 - x * x)); }
static inline double aci_acos(double x) { return aci_atan2(__builtin_sqrt(1.0 - x * x), x); }
static inline double aci_fmod(double x, double y) { return x - __builtin_trunc(x / y) * y; }
static inline double aci_fmin(double a, double b) { return a < b ? a : b; }
static inline double aci_fmax(double a, double b) { return a > b ? a : b; }

/* Standard names, so ordinary C DSP code compiles unchanged. */
#define sin(x)    aci_sin(x)
#define cos(x)    aci_cos(x)
#define tan(x)    aci_tan(x)
#define exp(x)    aci_exp(x)
#define exp2(x)   aci_exp2(x)
#define log(x)    aci_log(x)
#define log2(x)   aci_log2(x)
#define log10(x)  aci_log10(x)
#define pow(x, y) aci_pow(x, y)
#define tanh(x)   aci_tanh(x)
#define sinh(x)   aci_sinh(x)
#define cosh(x)   aci_cosh(x)
#define atan(x)   aci_atan(x)
#define atan2(y, x) aci_atan2(y, x)
#define asin(x)   aci_asin(x)
#define acos(x)   aci_acos(x)
#define fmod(x, y) aci_fmod(x, y)
#define fmin(a, b) aci_fmin(a, b)
#define fmax(a, b) aci_fmax(a, b)
#define sqrt(x)   __builtin_sqrt(x)
#define fabs(x)   __builtin_fabs(x)
#define floor(x)  __builtin_floor(x)
#define ceil(x)   __builtin_ceil(x)
#define trunc(x)  __builtin_trunc(x)
#define round(x)  aci__round(x)

#define sinf(x)   ((float)aci_sin(x))
#define cosf(x)   ((float)aci_cos(x))
#define tanf(x)   ((float)aci_tan(x))
#define expf(x)   ((float)aci_exp(x))
#define exp2f(x)  ((float)aci_exp2(x))
#define logf(x)   ((float)aci_log(x))
#define log2f(x)  ((float)aci_log2(x))
#define log10f(x) ((float)aci_log10(x))
#define powf(x, y) ((float)aci_pow(x, y))
#define tanhf(x)  ((float)aci_tanh(x))
#define sinhf(x)  ((float)aci_sinh(x))
#define coshf(x)  ((float)aci_cosh(x))
#define atanf(x)  ((float)aci_atan(x))
#define atan2f(y, x) ((float)aci_atan2(y, x))
#define asinf(x)  ((float)aci_asin(x))
#define acosf(x)  ((float)aci_acos(x))
#define fmodf(x, y) ((float)aci_fmod(x, y))
#define fminf(a, b) ((float)aci_fmin(a, b))
#define fmaxf(a, b) ((float)aci_fmax(a, b))
#define sqrtf(x)  __builtin_sqrtf(x)
#define fabsf(x)  __builtin_fabsf(x)
#define floorf(x) __builtin_floorf(x)
#define ceilf(x)  __builtin_ceilf(x)
#define truncf(x) __builtin_truncf(x)
#define roundf(x) ((float)aci__round(x))

/* ------------------------------------------------------------------------- */
/* Helpers (header-only, optional)                                           */
/* ------------------------------------------------------------------------- */

#define ACI_PI    3.14159265358979f
#define ACI_TWOPI 6.28318530717959f

static inline int aci_strlen(const char* s) { int n = 0; while (s[n]) ++n; return n; }
/* Debug log, visible in the host's log (rack.logs API / `aicontact.py logs`). */
static inline void aci_print(const char* msg) { aci_print_n(msg, aci_strlen(msg)); }

static inline float aci_clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float aci_lerp(float a, float b, float t) { return a + (b - a) * t; }
/* MIDI key -> Hz (A4 = key 69 = 440 Hz). */
static inline float aci_mtof(float key) { return 440.0f * powf(2.0f, (key - 69.0f) / 12.0f); }
static inline float aci_db_to_gain(float db) { return powf(10.0f, db / 20.0f); }

/* Fast white noise in [-1, 1). Keep one uint32 state per generator. */
static inline float aci_noise(unsigned int* state) {
    *state = *state * 1664525u + 1013904223u;
    return (float)(int)*state * (1.0f / 2147483648.0f);
}

/* Polynomial (PolyBLEP) band-limited saw / square. phase in [0,1), dt = freq/sr. */
static inline float aci_polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
static inline float aci_saw(float phase, float dt) { return 2.0f * phase - 1.0f - aci_polyblep(phase, dt); }
static inline float aci_square(float phase, float dt) {
    float p2 = phase + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
    return (phase < 0.5f ? 1.0f : -1.0f) + aci_polyblep(phase, dt) - aci_polyblep(p2, dt);
}

/* Linear-segment ADSR. Times in seconds, sustain 0..1. */
typedef struct { int stage; float level, a, d, s, r, sr; } aci_adsr;
enum { ACI_ADSR_IDLE = 0, ACI_ADSR_ATTACK, ACI_ADSR_DECAY, ACI_ADSR_SUSTAIN, ACI_ADSR_RELEASE };
static inline void aci_adsr_set(aci_adsr* e, float sr, float a, float d, float s, float r) {
    e->sr = sr; e->a = a; e->d = d; e->s = s; e->r = r;
}
static inline void aci_adsr_gate(aci_adsr* e, int on) { e->stage = on ? ACI_ADSR_ATTACK : (e->stage ? ACI_ADSR_RELEASE : ACI_ADSR_IDLE); }
static inline float aci_adsr_next(aci_adsr* e) {
    switch (e->stage) {
    case ACI_ADSR_ATTACK:
        e->level += 1.0f / (fmaxf(e->a, 0.0005f) * e->sr);
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = ACI_ADSR_DECAY; }
        break;
    case ACI_ADSR_DECAY:
        e->level -= (1.0f - e->s) / (fmaxf(e->d, 0.0005f) * e->sr);
        if (e->level <= e->s) { e->level = e->s; e->stage = ACI_ADSR_SUSTAIN; }
        break;
    case ACI_ADSR_SUSTAIN: e->level = e->s; break;
    case ACI_ADSR_RELEASE:
        e->level -= 1.0f / (fmaxf(e->r, 0.0005f) * e->sr);
        if (e->level <= 0.0f) { e->level = 0.0f; e->stage = ACI_ADSR_IDLE; }
        break;
    default: e->level = 0.0f; break;
    }
    return e->level;
}

/* Topology-preserving state-variable filter (Cytomic / Simper). */
typedef struct { float ic1, ic2; } aci_svf;
typedef struct { float lp, bp, hp; } aci_svf_out;
static inline aci_svf_out aci_svf_tick(aci_svf* f, float x, float cutoff_hz, float q, float sr) {
    float g = tanf(ACI_PI * aci_clamp(cutoff_hz, 10.0f, sr * 0.49f) / sr);
    float k = 1.0f / fmaxf(q, 0.05f);
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - f->ic2;
    float v1 = a1 * f->ic1 + a2 * v3;
    float v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    aci_svf_out o = { v2, v1, x - k * v1 - v2 };
    return o;
}

/* Simple voice allocator: returns a voice index for `key` (oldest is stolen). */
typedef struct { int key; int active; unsigned int age; } aci_voice_slot;
static inline int aci_voice_alloc(aci_voice_slot* v, int n, int key, unsigned int* clock) {
    int best = 0; unsigned int best_age = 0xffffffffu;
    for (int i = 0; i < n; ++i) if (!v[i].active) { best = i; best_age = 0; break; }
    if (best_age) for (int i = 0; i < n; ++i) if (v[i].age < best_age) { best = i; best_age = v[i].age; }
    v[best].key = key; v[best].active = 1; v[best].age = ++*clock;
    return best;
}

#ifdef __cplusplus
}
#endif
#endif /* ACI_H */
