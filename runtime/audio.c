/* Audio: the Xbox DirectSound API implemented as a software mixer.
 *
 * The XDK's DSOUND library drives the MCPX APU (voice processor, DSPs, scatter-gather tables), which is not
 * modelled. Instead the game's calls into DSOUND (and the movie player's calls through the stream vtable) are
 * handled here: every buffer / stream becomes a voice that the mixer renders at 48 kHz stereo straight from guest
 * memory (the game streams music by rewriting a looping ring while it plays, so data is never copied).
 *
 * The mixer is a plain function, audio_mix(); it is driven by a CoreAudio render callback, or by a pacing thread
 * when there is no output device (STW_HEADLESS, or CoreAudio unavailable) so voice positions advance in real
 * time either way. Debug knobs: STW_AUDIODUMP=<file.wav> writes whatever is mixed; STW_AUDIOTRACE=1 logs the API traffic
 * with the audio clock; STW_AUDIOSAVE=<prefix> saves the raw data of each streaming ring when it starts playing;
 * STW_NOAUDIO=1 keeps the mixer on its timer thread instead of opening the output device. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <AudioToolbox/AudioToolbox.h>
#include "kernel.h"
#include "lib.h"

#define RATE 48000
#define MAXV 192
#define MAXPKT 64
enum { VT_DIRECTSOUND = 0x2b16fc, VT_BUFFER = 0x2b1740, VT_STREAM = 0x2b174c };
enum { K_BUFFER = 1, K_STREAM = 2 };
#define OBJ_MAGIC 0x43565344u             /* 'DSVC' */
/* Guest RAM is mapped at 0 and again (physical alias) at 0x80000000; the game hands DSOUND the alias for contiguous buffers. */
static int ram_ok(uint32_t a, uint32_t n) {
    uint32_t lo = a & 0x7FFFFFFFu;
    return (a < 0x10000000u || (a >= 0x80000000u && a < 0x90000000u)) && (uint64_t)lo + n <= 0x10000000u;
}
#define MAX_GUEST_ADDR 0x10000000u        /* object pointers beyond this are garbage, not RAM */

typedef struct { uint32_t addr, size, done_size_ptr, status_ptr, event, consumed; } Packet;

typedef struct {
    int used, kind;
    uint32_t obj;
    int tag, nch, rate, bits, align;      /* WAVEFORMATEX: tag 1 = PCM, 0x69 = Xbox ADPCM */
    int ctrl3d;
    float gl, gr;                         /* 2D mix-bin gains (front left / right) */
    float p3[3], mind, maxd;             /* DS3D buffer parameters */
    /* buffers */
    uint32_t data, bytes, pstart, plen;   /* play region in bytes; plen 0 = to the end */
    int playing, paused, looping;
    double pos;                           /* in source frames from `data` (streams: from the head packet) */
    int32_t vol_mb, pitch, have_pitch;
    float gain;
    /* ADPCM decode cache (two blocks so linear interpolation across a block edge does not thrash) */
    int64_t cblock[2];
    int16_t cache[2][2][64];
    uint32_t cbase;                       /* guest address the cache was decoded from */
    /* streams */
    Packet q[MAXPKT];
    int qhead, qcount, maxpackets;
} Voice;

static Voice voices[MAXV];
static pthread_mutex_t amu = PTHREAD_MUTEX_INITIALIZER;
static int trace;
static uint64_t mixed_frames;
#define TR(...) do { if (trace) { fprintf(stderr, "[audio %.2f] ", (double)mixed_frames / RATE); fprintf(stderr, __VA_ARGS__); } } while (0)

/* ---- completions are reported to the guest from a service thread (writing status words and signalling
 *      events needs the guest lock, which the real-time mixer must not wait for) ---- */
typedef struct { uint32_t size_ptr, size, status_ptr, status, event; } Completion;
#define MAXCOMP 256
static Completion comps[MAXCOMP];
static int comp_head, comp_tail;
static pthread_mutex_t cmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ccv = PTHREAD_COND_INITIALIZER;

static void complete(const Packet *p, uint32_t size, uint32_t status) {
    pthread_mutex_lock(&cmu);
    int n = (comp_tail + 1) % MAXCOMP;
    if (n != comp_head) {
        comps[comp_tail] = (Completion){ p->done_size_ptr, size, p->status_ptr, status, p->event };
        comp_tail = n;
        pthread_cond_signal(&ccv);
    }
    pthread_mutex_unlock(&cmu);
}
static void *comp_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&cmu);
        while (comp_head == comp_tail) pthread_cond_wait(&ccv, &cmu);
        Completion c = comps[comp_head];
        comp_head = (comp_head + 1) % MAXCOMP;
        pthread_mutex_unlock(&cmu);
        gil_lock_priority();
        if (c.size_ptr) ST32(c.size_ptr, c.size);
        if (c.status_ptr) ST32(c.status_ptr, c.status);
        if (c.event) kobj_signal_handle(c.event);
        gil_release();
    }
    return NULL;
}

/* ---- ADPCM ---- */
static const int8_t idx_tab[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t step_tab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

/* One 36-byte-per-channel block = 64 frames. Stereo blocks are the standard IMA layout: both channel headers first
 * (int16 predictor, uint8 step index, pad), then 8 groups of 4 bytes (8 nibbles) per channel, interleaved. The header
 * predictor is frame 0, the first 63 nibbles are frames 1..63, and the 64th nibble is always zero padding (checked
 * against the game's own music data: it is 0 in every block, and the next block's header continues after nibble 62). */
static void adpcm_block(const uint8_t *blk, int nch, int16_t out[2][64]) {
    for (int ch = 0; ch < nch; ch++) {
        const uint8_t *h = blk + ch * 4;
        int pred = (int16_t)(h[0] | (h[1] << 8)), idx = h[2];
        if (idx > 88) idx = 88;
        out[ch][0] = (int16_t)pred;
        for (int n = 0; n < 63; n++) {
            const uint8_t *d = blk + nch * 4 + (n / 8) * nch * 4 + ch * 4;
            int nib = (d[(n % 8) / 2] >> (4 * (n & 1))) & 15, step = step_tab[idx], diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            if (nib & 8) diff = -diff;
            pred += diff;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            idx += idx_tab[nib];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            out[ch][n + 1] = (int16_t)pred;
        }
    }
}

static int bytes_per_frame(const Voice *v) { return v->nch * (v->bits >= 16 ? 2 : 1); }
static uint32_t block_bytes(const Voice *v) { return 36u * (uint32_t)v->nch; }
static int is_adpcm(const Voice *v) { return v->tag == 0x69; }
static double bytes_to_frames(const Voice *v, uint32_t b) {
    return is_adpcm(v) ? (double)(b / block_bytes(v)) * 64.0 : (double)(b / (uint32_t)bytes_per_frame(v));
}
static uint32_t frames_to_bytes(const Voice *v, double f) {
    return is_adpcm(v) ? (uint32_t)((int64_t)f / 64) * block_bytes(v) : (uint32_t)((int64_t)f * bytes_per_frame(v));
}
static int valid_format(const Voice *v) {
    if (v->nch < 1 || v->nch > 2 || v->rate < 1000 || v->rate > 200000) return 0;
    if (is_adpcm(v)) return 1;
    return v->tag == 1 && (v->bits == 8 || v->bits == 16);
}

/* Frame i of the data at guest address `base` (`bytes` long) into lr; zero outside. */
static void fetch(Voice *v, uint32_t base, uint32_t bytes, int64_t i, float lr[2]) {
    lr[0] = lr[1] = 0;
    if (i < 0) return;
    if (is_adpcm(v)) {
        int64_t blk = i / 64;
        if ((uint64_t)(blk + 1) * block_bytes(v) > bytes) return;
        if (v->cbase != base) { v->cblock[0] = v->cblock[1] = -1; v->cbase = base; }
        int s = (int)(blk & 1);
        if (v->cblock[s] != blk) {
            adpcm_block(G + base + (uint32_t)blk * block_bytes(v), v->nch, v->cache[s]);
            v->cblock[s] = blk;
        }
        lr[0] = v->cache[s][0][i % 64] / 32768.0f;
        lr[1] = v->nch > 1 ? v->cache[s][1][i % 64] / 32768.0f : lr[0];
        return;
    }
    int bpf = bytes_per_frame(v);
    if ((uint64_t)(i + 1) * (uint64_t)bpf > bytes) return;
    const uint8_t *p = G + base + (uint32_t)i * (uint32_t)bpf;
    if (v->bits == 16) {
        int16_t a, b;
        memcpy(&a, p, 2);
        b = v->nch > 1 ? (memcpy(&b, p + 2, 2), b) : a;
        lr[0] = a / 32768.0f; lr[1] = b / 32768.0f;
    } else {
        lr[0] = ((int)p[0] - 128) / 128.0f;
        lr[1] = v->nch > 1 ? ((int)p[1] - 128) / 128.0f : lr[0];
    }
}

static double step_of(const Voice *v) {
    double src = v->have_pitch ? RATE * pow(2.0, v->pitch / 4096.0) : (double)v->rate;
    return src / RATE;
}

static void buffer_region(const Voice *v, double *rs, double *re) {
    double total = bytes_to_frames(v, v->bytes);
    *rs = bytes_to_frames(v, v->pstart);
    *re = v->plen ? *rs + bytes_to_frames(v, v->plen) : total;
    if (*rs > total) *rs = total;
    if (*re > total) *re = total;
}

static float lis_pos[3], lis_front[3] = { 0, 0, 1 }, lis_top[3] = { 0, 1, 0 };

/* Output gains for the voice's left/right channels. Mono sources are placed: 3D voices by their position relative to the
 * listener (inverse-distance rolloff between the min and max distance, equal-power pan on the listener's right axis),
 * 2D voices by their front-left/front-right mix-bin volumes. */
static void voice_gains(const Voice *v, float *cl, float *cr) {
    *cl = *cr = v->gain;
    if (v->ctrl3d) {
        float rel[3] = { v->p3[0] - lis_pos[0], v->p3[1] - lis_pos[1], v->p3[2] - lis_pos[2] };
        float d = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        float dd = d > v->maxd ? v->maxd : d, att = dd > v->mind && v->mind > 0 ? v->mind / dd : 1.0f;
        *cl *= att; *cr *= att;
        if (v->nch == 1 && d > 1e-4f) {
            float right[3] = { lis_top[1] * lis_front[2] - lis_top[2] * lis_front[1], lis_top[2] * lis_front[0] - lis_top[0] * lis_front[2],
                               lis_top[0] * lis_front[1] - lis_top[1] * lis_front[0] };
            float pan = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / d;
            if (pan > 1) pan = 1;
            if (pan < -1) pan = -1;
            float ang = (pan + 1.0f) * 0.78539816f;
            *cl *= 1.41421356f * cosf(ang); *cr *= 1.41421356f * sinf(ang);
        }
    } else if (v->nch == 1) { *cl *= v->gl; *cr *= v->gr; }
}

static void mix_buffer(Voice *v, float *out, int n) {
    if (!v->data || !v->bytes || !ram_ok(v->data, v->bytes) || !valid_format(v)) return;
    double rs, re, st = step_of(v);
    float cl, cr;
    voice_gains(v, &cl, &cr);
    buffer_region(v, &rs, &re);
    v->cblock[0] = v->cblock[1] = -1;                       /* the game rewrites ring buffers as they play */
    for (int k = 0; k < n && v->playing; k++) {
        if (v->pos >= re) {
            if (v->looping && re > rs) v->pos = rs + fmod(v->pos - re, re - rs);
            else { v->playing = 0; v->pos = re; break; }
        }
        int64_t i = (int64_t)v->pos;
        double fr = v->pos - i;
        float a[2], b[2];
        fetch(v, v->data, v->bytes, i, a);
        fetch(v, v->data, v->bytes, i + 1, b);
        out[2 * k] += (a[0] + (b[0] - a[0]) * (float)fr) * cl;
        out[2 * k + 1] += (a[1] + (b[1] - a[1]) * (float)fr) * cr;
        v->pos += st;
    }
}

static void mix_stream(Voice *v, float *out, int n) {
    if (!valid_format(v)) return;
    double st = step_of(v);
    v->cblock[0] = v->cblock[1] = -1;
    for (int k = 0; k < n && v->qcount > 0; k++) {
        Packet *p = &v->q[v->qhead];
        double frames = bytes_to_frames(v, p->size);
        if (v->pos >= frames) {                             /* head packet fully played */
            complete(p, p->size, 0);
            v->pos -= frames;
            v->qhead = (v->qhead + 1) % MAXPKT;
            v->qcount--;
            v->cbase = 0;
            k--;
            continue;
        }
        if (!ram_ok(p->addr, p->size)) { v->pos = frames; k--; continue; }
        int64_t i = (int64_t)v->pos;
        double fr = v->pos - i;
        float a[2], b[2];
        fetch(v, p->addr, p->size, i, a);
        fetch(v, p->addr, p->size, i + 1, b);
        out[2 * k] += (a[0] + (b[0] - a[0]) * (float)fr) * v->gain;
        out[2 * k + 1] += (a[1] + (b[1] - a[1]) * (float)fr) * v->gain;
        v->pos += st;
    }
}

/* ---- WAV dump ---- */
static FILE *wav;
static uint32_t wav_frames;
static void wav_header(void) {
    uint32_t dsz = wav_frames * 4, rsz = 36 + dsz, fmt = 16, rate = RATE, brate = RATE * 4;
    uint16_t tag = 1, ch = 2, al = 4, bits = 16;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&rsz, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav); fwrite(&fmt, 4, 1, wav);
    fwrite(&tag, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav); fwrite(&brate, 4, 1, wav);
    fwrite(&al, 2, 1, wav); fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&dsz, 4, 1, wav);
    fseek(wav, 0, SEEK_END);
    fflush(wav);
}

void audio_mix(float *out, int n) {
    memset(out, 0, (size_t)n * 2 * sizeof(float));
    pthread_mutex_lock(&amu);
    for (int i = 0; i < MAXV; i++) {
        Voice *v = &voices[i];
        if (!v->used) continue;
        if (v->kind == K_BUFFER && v->playing && !v->paused) mix_buffer(v, out, n);
        else if (v->kind == K_STREAM && !v->paused) mix_stream(v, out, n);
    }
    mixed_frames += (uint64_t)n;
    pthread_mutex_unlock(&amu);
    for (int i = 0; i < n * 2; i++) {                       /* soft knee above 0.9 so loud overlaps do not hard-clip */
        float x = out[i], a = fabsf(x);
        if (a > 0.9f) out[i] = copysignf(0.9f + 0.1f * tanhf((a - 0.9f) / 0.1f), x);
    }
    if (wav) {
        int16_t tmp[2048];
        for (int off = 0; off < n * 2; off += 2048) {
            int m = n * 2 - off < 2048 ? n * 2 - off : 2048;
            for (int i = 0; i < m; i++) tmp[i] = (int16_t)lrintf(out[off + i] * 32767.0f);
            fwrite(tmp, 2, (size_t)m, wav);
        }
        wav_frames += (uint32_t)n;
        if ((wav_frames & 0x3FFF) < (uint32_t)n) wav_header();
    }
}

/* ---- output ---- */
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)fl; (void)ts; (void)bus;
    audio_mix((float *)io->mBuffers[0].mData, (int)frames);
    return noErr;
}
static void *pace_thread(void *arg) {                      /* no output device: mix in real time anyway */
    (void)arg;
    static float buf[480 * 2];
    uint64_t next = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (;;) {
        next += 10 * 1000 * 1000;
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (next > now) {
            struct timespec t = { (time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull) };
            nanosleep(&t, NULL);
        } else if (now - next > 200 * 1000 * 1000) next = now;      /* stalled (suspended): do not burst to catch up */
        audio_mix(buf, 480);
    }
    return NULL;
}
static void wav_close(void) { if (wav) { wav_header(); fclose(wav); wav = NULL; } }

static void audio_init(void) {
    static int done;
    if (done) return;
    done = 1;
    trace = getenv("STW_AUDIOTRACE") != NULL;
    const char *dump = getenv("STW_AUDIODUMP");
    if (dump && (wav = fopen(dump, "wb"))) { wav_header(); atexit(wav_close); }
    pthread_t th;
    pthread_create(&th, NULL, comp_thread, NULL);
    pthread_detach(th);
    int device = 0;
    if (!getenv("STW_HEADLESS") && !getenv("STW_NOAUDIO")) {
        AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent comp = AudioComponentFindNext(NULL, &d);
        AudioUnit unit;
        AudioStreamBasicDescription f = { RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
        AURenderCallbackStruct cb = { render_cb, NULL };
        if (comp && AudioComponentInstanceNew(comp, &unit) == noErr &&
            AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) == noErr &&
            AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) == noErr &&
            AudioUnitInitialize(unit) == noErr && AudioOutputUnitStart(unit) == noErr)
            device = 1;
    }
    if (!device) {
        pthread_create(&th, NULL, pace_thread, NULL);
        pthread_detach(th);
    }
    TR("mixer started (%s)\n", device ? "CoreAudio" : "timer thread");
}

/* ---- guest objects ---- */
static uint32_t new_object(uint32_t vtbl, int voice) {
    uint32_t o = guest_alloc(64, 16);
    memset(G + o, 0, 64);
    ST32(o, vtbl);
    ST32(o + 4, 1);
    ST32(o + 8, (uint32_t)voice);
    ST32(o + 12, OBJ_MAGIC);
    return o;
}
static Voice *vof(uint32_t obj) {
    if (obj < 0x1000 || obj >= MAX_GUEST_ADDR || LD32(obj + 12) != OBJ_MAGIC) return NULL;
    uint32_t i = LD32(obj + 8);
    return i < MAXV && voices[i].used ? &voices[i] : NULL;
}
static int alloc_voice(int kind, uint32_t wfx) {
    for (int i = 0; i < MAXV; i++)
        if (!voices[i].used) {
            Voice *v = &voices[i];
            memset(v, 0, sizeof *v);
            v->used = 1; v->kind = kind; v->gain = 1.0f; v->gl = v->gr = 1.0f; v->mind = 1.0f; v->maxd = 1e9f; v->cblock[0] = v->cblock[1] = -1;
            if (wfx && wfx < MAX_GUEST_ADDR) {
                v->tag = LD16(wfx); v->nch = LD16(wfx + 2); v->rate = (int)LD32(wfx + 4); v->align = LD16(wfx + 12); v->bits = LD16(wfx + 14);
            }
            return i;
        }
    return -1;
}

uint32_t lib_DirectSoundCreate(CPU *c, const uint32_t *a) {      /* (LPGUID, LPDIRECTSOUND *ppDS, LPUNKNOWN) */
    (void)c;
    audio_init();
    if (a[1]) ST32(a[1], new_object(VT_DIRECTSOUND, -1));
    return 0;
}
uint32_t lib_DS_CreateSoundBuffer(CPU *c, const uint32_t *a) {   /* (pds, DSBUFFERDESC *, IDirectSoundBuffer **, pUnk) */
    (void)c;
    uint32_t d = a[1];
    pthread_mutex_lock(&amu);
    int i = alloc_voice(K_BUFFER, d ? LD32(d + 12) : 0);
    if (i >= 0) {
        voices[i].ctrl3d = d && (LD32(d + 4) & 0x10);
        if (d && LD32(d + 8)) {                             /* the driver owns the memory: not used by this game, allocate it anyway */
            voices[i].bytes = LD32(d + 8);
            voices[i].data = guest_alloc(voices[i].bytes, 16);
            memset(G + voices[i].data, 0, voices[i].bytes);
        }
        TR("CreateSoundBuffer -> voice %d  flags=%#x bytes=%u tag=%#x ch=%d rate=%d bits=%d align=%d\n", i, d ? LD32(d + 4) : 0, d ? LD32(d + 8) : 0,
           voices[i].tag, voices[i].nch, voices[i].rate, voices[i].bits, voices[i].align);
    }
    pthread_mutex_unlock(&amu);
    if (i < 0) return 0x8007000E;                           /* E_OUTOFMEMORY */
    if (a[2]) ST32(a[2], new_object(VT_BUFFER, i));
    return 0;
}
uint32_t lib_DS_CreateStream(CPU *c, const uint32_t *a) {        /* DirectSoundCreateStream(DSSTREAMDESC *, IDirectSoundStream **) */
    (void)c;
    audio_init();
    uint32_t d = a[0];
    pthread_mutex_lock(&amu);
    int i = alloc_voice(K_STREAM, d ? LD32(d + 8) : 0);
    if (i >= 0) {
        voices[i].maxpackets = d ? (int)LD32(d + 4) : 0;
        if (voices[i].maxpackets < 1 || voices[i].maxpackets > MAXPKT) voices[i].maxpackets = MAXPKT;
        TR("CreateStream -> voice %d flags=%#x maxpackets=%d tag=%#x ch=%d rate=%d bits=%d align=%d\n", i, d ? LD32(d) : 0, voices[i].maxpackets,
           voices[i].tag, voices[i].nch, voices[i].rate, voices[i].bits, voices[i].align);
    }
    pthread_mutex_unlock(&amu);
    if (i < 0) return 0x8007000E;
    if (a[1]) ST32(a[1], new_object(VT_STREAM, i));
    return 0;
}

/* ---- IDirectSoundBuffer ---- */
static float gain_of(int32_t mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }

uint32_t lib_DS_SetVolume(CPU *c, const uint32_t *a) {           /* (pBuffer, lVolume) in 1/100 dB */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) { v->vol_mb = (int32_t)a[1]; v->gain = gain_of(v->vol_mb); }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_SetPitch(CPU *c, const uint32_t *a) {            /* (pBuffer, lPitch): 4096 per octave relative to 48 kHz */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) { v->pitch = (int32_t)a[1]; v->have_pitch = 1; }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_SetBufferData(CPU *c, const uint32_t *a) {       /* (pBuffer, pvData, dwBufferBytes) */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) {
        v->data = a[1]; v->bytes = a[2]; v->pstart = 0; v->plen = 0;
        if (!v->playing) v->pos = 0;
        TR("SetBufferData voice %d data=%#x bytes=%u\n", (int)(v - voices), a[1], a[2]);
    }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_SetPlayRegion(CPU *c, const uint32_t *a) {       /* (pBuffer, dwPlayStart, dwPlayLength) in bytes */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) { v->pstart = a[1]; v->plen = a[2]; TR("SetPlayRegion voice %d start=%u len=%u\n", (int)(v - voices), a[1], a[2]); }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_Play(CPU *c, const uint32_t *a) {                /* (pBuffer, res1, res2, dwFlags): 1 = looping, 2 = from start */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v && v->kind == K_BUFFER) {
        double rs, re;
        buffer_region(v, &rs, &re);
        if ((a[3] & 2) || !v->playing) {
            if ((a[3] & 2) || v->pos >= re || v->pos < rs) v->pos = rs;
        }
        if (getenv("STW_AUDIOSAVE") && v->bytes >= 0x40000) {     /* debug: raw ADPCM of a streaming ring, for offline analysis */
            static int n; char name[128];
            snprintf(name, sizeof name, "%s.%d", getenv("STW_AUDIOSAVE"), n++);
            FILE *f = fopen(name, "wb");
            if (f) { fwrite(G + v->data, 1, v->bytes, f); fclose(f); }
        }
        v->looping = a[3] & 1;
        v->playing = 1;
        v->paused = 0;
        TR("Play voice %d flags=%#x data=%#x bytes=%u region=%u+%u pos=%.0f gain=%.3f step=%.3f tag=%#x ch=%d rate=%d\n", (int)(v - voices), a[3], v->data, v->bytes,
           v->pstart, v->plen, v->pos, v->gain, step_of(v), v->tag, v->nch, v->rate);
    }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_Stop(CPU *c, const uint32_t *a) {                /* (pBuffer) */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v && v->kind == K_BUFFER) { if (v->playing) TR("Stop voice %d\n", (int)(v - voices)); v->playing = 0; v->paused = 0; }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_Pause(CPU *c, const uint32_t *a) {               /* (pBuffer, dwPause) */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) v->paused = a[1] != 0;
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_GetStatus(CPU *c, const uint32_t *a) {           /* (pBuffer, pdwStatus): 1 playing, 2 paused, 4 looping */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    uint32_t s = 0;
    if (v && v->kind == K_BUFFER && v->playing) s = 1 | (v->paused ? 2 : 0) | (v->looping ? 4 : 0);
    pthread_mutex_unlock(&amu);
    if (a[1]) ST32(a[1], s);
    return 0;
}
uint32_t lib_DS_GetCurrentPosition(CPU *c, const uint32_t *a) {  /* (pBuffer, pdwPlay, pdwWrite): byte offsets into the buffer */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    uint32_t p = v ? frames_to_bytes(v, v->pos) : 0;
    pthread_mutex_unlock(&amu);
    if (a[1]) ST32(a[1], p);
    if (a[2]) ST32(a[2], p);
    return 0;
}
uint32_t lib_DS_AddRef(CPU *c, const uint32_t *a) {
    (void)c;
    if (a[0] < 0x1000 || a[0] >= MAX_GUEST_ADDR) return 0;
    ST32(a[0] + 4, LD32(a[0] + 4) + 1);
    return LD32(a[0] + 4);
}
uint32_t lib_DS_Release(CPU *c, const uint32_t *a) {             /* any DirectSound object made here: frees the voice at zero */
    (void)c;
    if (a[0] < 0x1000 || a[0] >= MAX_GUEST_ADDR || LD32(a[0] + 12) != OBJ_MAGIC) return 0;
    uint32_t r = LD32(a[0] + 4);
    if (r) ST32(a[0] + 4, --r);
    if (!r) {
        pthread_mutex_lock(&amu);
        Voice *v = vof(a[0]);
        if (v) {
            while (v->qcount > 0) { complete(&v->q[v->qhead], 0, 0x80004004); v->qhead = (v->qhead + 1) % MAXPKT; v->qcount--; }
            v->used = 0;
        }
        pthread_mutex_unlock(&amu);
        ST32(a[0] + 12, 0);
    }
    return r;
}

/* ---- IDirectSoundStream (the movie player's audio) ---- */
uint32_t lib_DS_StreamProcess(CPU *c, const uint32_t *a) {       /* (pStream, XMEDIAPACKET *src, XMEDIAPACKET *dst) */
    (void)c;
    uint32_t pk = a[1];
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    uint32_t r = 0;
    if (!v || v->kind != K_STREAM || !pk) r = 0x80004005;
    else if (v->qcount >= v->maxpackets) r = 0x8007000E;
    else {
        Packet *p = &v->q[(v->qhead + v->qcount) % MAXPKT];
        *p = (Packet){ LD32(pk), LD32(pk + 4), LD32(pk + 8), LD32(pk + 12), LD32(pk + 16), 0 };
        v->qcount++;
        TR("Process voice %d packet addr=%#x size=%u status@%#x event=%#x queued=%d\n", (int)(v - voices), p->addr, p->size, p->status_ptr, p->event, v->qcount);
        if (p->status_ptr) ST32(p->status_ptr, 0x8000000A);   /* XMEDIAPACKET_STATUS_PENDING */
        if (p->done_size_ptr) ST32(p->done_size_ptr, 0);
    }
    pthread_mutex_unlock(&amu);
    return r;
}
uint32_t lib_DS_StreamGetStatus(CPU *c, const uint32_t *a) {     /* (pStream, pdwStatus): 1 ready for a packet, 2 paused */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    uint32_t s = 0;
    if (v) s = (v->qcount < v->maxpackets ? 1 : 0) | (v->paused ? 2 : 0);
    pthread_mutex_unlock(&amu);
    if (a[1]) ST32(a[1], s);
    return 0;
}
uint32_t lib_DS_StreamFlush(CPU *c, const uint32_t *a) {         /* (pStream): pending packets complete as flushed */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) {
        while (v->qcount > 0) { complete(&v->q[v->qhead], 0, 0x80004004); v->qhead = (v->qhead + 1) % MAXPKT; v->qcount--; }
        v->pos = 0;
    }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_StreamGetInfo(CPU *c, const uint32_t *a) {       /* (pStream, XMEDIAINFO *) */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    uint32_t align = v && v->align ? (uint32_t)v->align : 1;
    pthread_mutex_unlock(&amu);
    if (a[1]) { ST32(a[1], 0x1); ST32(a[1] + 4, align); ST32(a[1] + 8, 0); ST32(a[1] + 12, 0); }   /* flags, input size, output size, lookahead */
    return 0;
}
/* ---- 3D and mix bins ---- */
static float fa(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
uint32_t lib_DS_ListenerSetPosition(CPU *c, const uint32_t *a) {   /* (pds, x, y, z, dwApply): applied immediately */
    (void)c;
    pthread_mutex_lock(&amu);
    for (int i = 0; i < 3; i++) lis_pos[i] = fa(a[1 + i]);
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_ListenerSetOrientation(CPU *c, const uint32_t *a) {   /* (pds, fx, fy, fz, tx, ty, tz, dwApply) */
    (void)c;
    pthread_mutex_lock(&amu);
    for (int i = 0; i < 3; i++) { lis_front[i] = fa(a[1 + i]); lis_top[i] = fa(a[4 + i]); }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_BufferSetPosition(CPU *c, const uint32_t *a) {     /* (pBuffer, x, y, z, dwApply) */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) for (int i = 0; i < 3; i++) v->p3[i] = fa(a[1 + i]);
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_BufferSetMinDistance(CPU *c, const uint32_t *a) {  /* (pBuffer, flDistance, dwApply) */
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) v->mind = fa(a[1]);
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_BufferSetMaxDistance(CPU *c, const uint32_t *a) {
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v) v->maxd = fa(a[1]);
    pthread_mutex_unlock(&amu);
    return 0;
}
/* IDirectSoundBuffer::SetMixBins(DSMIXBINS *): { dwCount, DSMIXBINVOLUMEPAIR *{ dwMixBin, lVolume } }. Bins 0/1 are front
 * left/right, 2 centre, 4/5 back left/right: everything is folded down to stereo. */
uint32_t lib_DS_SetMixBins(CPU *c, const uint32_t *a) {
    (void)c;
    pthread_mutex_lock(&amu);
    Voice *v = vof(a[0]);
    if (v && a[1] && a[1] < MAX_GUEST_ADDR) {
        uint32_t n = LD32(a[1]), pp = LD32(a[1] + 4);
        float l = 0, r = 0;
        if (n <= 16 && pp && pp < MAX_GUEST_ADDR)
            for (uint32_t i = 0; i < n; i++) {
                uint32_t bin = LD32(pp + 8 * i);
                float g = gain_of((int32_t)LD32(pp + 8 * i + 4));
                if (bin == 0 || bin == 4) l += g;
                else if (bin == 1 || bin == 5) r += g;
                else if (bin == 2) { l += g * 0.7071f; r += g * 0.7071f; }
            }
        v->gl = l > 1 ? 1 : l; v->gr = r > 1 ? 1 : r;
        if (n == 0) v->gl = v->gr = 1.0f;
        TR("SetMixBins voice %d count=%u -> L %.3f R %.3f\n", (int)(v - voices), n, v->gl, v->gr);
    }
    pthread_mutex_unlock(&amu);
    return 0;
}
uint32_t lib_DS_Silent(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
