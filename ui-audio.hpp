#pragma once
// ── ui-audio.hpp ─────────────────────────────────────────────────────────────
// The board side of the sound engine (snd-engine.hpp and friends): the I2S
// driver, the audio task, the world snapshot the composer writes from, and
// the old k10Play() entry point, now routed into the engine.
//
// Signal path: snd-engine renders full-scale 16 kHz int16 blocks on core 0 ->
// sndOutput() applies the owner's volume and interpolates 3x in float, then
// cuts each sample to 16 bits, dithered and noise-shaped -> i2s_write() at
// 48 kHz -> the K10's digital class-D amp -> the 2 W speaker.
//
// The K10 library installs I2S0 at 16 kHz, 16-bit stereo, full duplex, with
// three 300-sample DMA buffers (56 ms). sndI2sInit() reinstalls it on the
// same pins, transmit-only, at 48 kHz with six 768-frame buffers (96 ms,
// 18 KB): enough slack that Wi-Fi bursts and flash writes do not starve the
// speaker. Nothing in this game reads the microphones. The old tone task
// retuned the peripheral to 8 kHz and back for every cue; nothing here
// touches the clock again after boot (except a GET /sndtest?fmt= A/B).
//
// Story beats are raised with sndStory() from drainEvents() and friends;
// see snd-engine.hpp for the list. k10Play(MOTIF_*) still works: each motif
// maps to its orchestrated effect, and the motifs that fire at a damage site
// whose event already carries a richer story sound (fire, lightning, flood,
// the tunnel hatch) map to nothing, so one beat never plays twice.

#include <esp_timer.h>
#include "snd-engine.hpp"

// Written by publishDread() (actions_game_loop.hpp) under G.mutex: how close
// the caravan is to the nearest survivor on the surface, 0..255.
static volatile uint8_t g_sndCaravanNear = 0;

static TaskHandle_t sndTaskH = nullptr;
static bool         sndOk    = false;
// The audio task's own accounting, read by the 30 s serial line.
static volatile uint32_t sndStUs = 0, sndStMaxUs = 0, sndStN = 0, sndStLate = 0;
static constexpr int SND_IO_FRAMES = 256;                      // 16 ms per i2s_write

// I2S formats the speaker can be A/B tested in without reflashing:
// GET /sndtest?fmt=N reinstalls in format N (from the audio task, between
// writes) and plays the test sequence. 4 is the K10 library's own install.
//
// os: the engine always renders at 16 kHz; os 2 or 3 runs the bus at 32 or
// 48 kHz and interpolates in software (sndOutput). At 16 kHz on the bus the
// board added a fizzing "hiss band" that followed the music, kept the same
// ratio to it at every volume, and was worst on the bright LPC voice, while
// a pure tone stayed clean: the amp's own reconstruction mirrors 1-5 kHz
// content up to 11-15 kHz (a tone's mirror lands at 15 kHz and up, out of
// earshot). 48 kHz was heard as "much better" (2026-09-23). The first
// listens also ruled out the bus format (Philips, MSB and PCM all sounded
// the same) and clipping. Every format is 16-bit samples in 16-bit slots:
// 32-bit slots (BCLK = 64 fs), tried so that 32-bit samples could carry the
// volume without losing resolution, play silence on this amp even with the
// low 16 bits zero -- it reads the slot's low half or does not take 64 fs.
// Never put data in that half: if the amp reads it, it is full-scale noise.
struct SndI2sFmt { const char* name; bool duplex; i2s_comm_format_t comm; uint8_t bufs; uint16_t len; uint8_t os; };
static const SndI2sFmt SND_FMT[] = {
  { "tx-i2s-6x256",      false, I2S_COMM_FORMAT_STAND_I2S,       6, 256, 1 },
  { "tx-msb-6x256",      false, I2S_COMM_FORMAT_STAND_MSB,       6, 256, 1 },
  { "duplex-i2s-6x256",  true,  I2S_COMM_FORMAT_STAND_I2S,       6, 256, 1 },
  { "duplex-msb-6x256",  true,  I2S_COMM_FORMAT_STAND_MSB,       6, 256, 1 },
  { "library-3x300",     true,  I2S_COMM_FORMAT_STAND_I2S,       3, 300, 1 },
  { "tx-pcmshort-6x256", false, I2S_COMM_FORMAT_STAND_PCM_SHORT, 6, 256, 1 },
  { "tx-i2s-x2-32k",     false, I2S_COMM_FORMAT_STAND_I2S,       6, 512, 2 },
  { "tx-i2s-x3-48k",     false, I2S_COMM_FORMAT_STAND_I2S,       6, 768, 3 },
};
static constexpr uint8_t SND_FMT_N = sizeof(SND_FMT) / sizeof(SND_FMT[0]);
static constexpr uint8_t SND_FMT_SAFE = 2;  // the 16 kHz duplex install: the fallback if the default will not come up
static volatile uint8_t sndFmtReq = 0;      // 1 + format index, set by GET /sndtest?fmt=N
static uint8_t sndFmt = 7;                  // 48 kHz, interpolated
static int64_t sndDmaUs = 96000;            // the installed format's DMA depth

// ── Oversampling: the engine's 16 kHz, interpolated to the bus rate ─────────
// Polyphase FIR, remez-designed: flat (0.02 dB) to 5.8 kHz, and the images of
// everything the engine makes gone by 10.2 kHz (-99 dB at 2x, -79 dB at 3x).
// Mono in, the same sample on both channels out.
static const float SND_OS2[2][20] = {   // 40 taps, pass 0-5.8 kHz, images from 10.2 kHz down 99 dB
  { -0.00001167f, 0.00065557f, -0.00237205f, 0.00616660f, -0.01324384f, 0.02513309f, -0.04405342f, 0.07459147f, -0.13177946f, 0.31424951f, 0.87568412f, -0.14717382f, 0.06150989f, -0.02809718f, 0.01192330f, -0.00405159f, 0.00065200f, 0.00042497f, -0.00047895f, 0.00027147f },
  { 0.00027147f, -0.00047895f, 0.00042497f, 0.00065200f, -0.00405159f, 0.01192330f, -0.02809718f, 0.06150989f, -0.14717382f, 0.87568412f, 0.31424951f, -0.13177946f, 0.07459147f, -0.04405342f, 0.02513309f, -0.01324384f, 0.00616660f, -0.00237205f, 0.00065557f, -0.00001167f },
};
static const float SND_OS3[3][16] = {   // 48 taps, pass 0-5.8 kHz, images from 10.2 kHz down 79 dB
  { -0.00007964f, 0.00286005f, -0.00805881f, 0.01780581f, -0.03362018f, 0.05824028f, -0.10045625f, 0.21609915f, 0.91874931f, -0.09458375f, 0.02902299f, -0.00690343f, -0.00128414f, 0.00329600f, -0.00267723f, 0.00160822f },
  { 0.00035954f, 0.00189084f, -0.00747768f, 0.01988887f, -0.04379012f, 0.08869438f, -0.18665130f, 0.62706705f, 0.62706705f, -0.18665130f, 0.08869438f, -0.04379012f, 0.01988887f, -0.00747768f, 0.00189084f, 0.00035954f },
  { 0.00160822f, -0.00267723f, 0.00329600f, -0.00128414f, -0.00690343f, 0.02902299f, -0.09458375f, 0.91874931f, 0.21609915f, -0.10045625f, 0.05824028f, -0.03362018f, 0.01780581f, -0.00805881f, 0.00286005f, -0.00007964f },
};
static float    sndOsH[2 * 20];                    // input history, doubled so every read is contiguous
static int      sndOsP = 0;
static int16_t  sndOutBuf[2 * SND_IO_FRAMES * 3];  // one i2s_write at the bus rate

// ── The output stage: volume, interpolation, the 16-bit cut ─────────────────
// The engine hands over full-scale audio (SM.vol stays 1 on the board). The
// owner's volume and the interpolation run here in float, and only then is
// each sample cut to the bus's 16 bits. The amp is so hot that the volume is
// a gain of a few hundredths, and the engine used to apply it and then
// truncate: at the old volume 3 (0.02) the loudest sample was +-464 steps
// and a soft consonant or the ambient bed sat on a handful of them -- 30 dB
// SNR offline, with an error that tracked the signal: grit that followed the
// content. TPDF dither turns that into a steady hiss and 2nd-order shaping
// moves most of the hiss above the engine's band (48 kHz leaves 8-24 kHz for
// it): 42 dB, uncorrelated, and on the board the new cut was heard as
// cleaner (2026-09-23). More bits would do better still, but this amp takes
// only 16-bit slots (SND_FMT above).
//   q 0  the old cut, for A/B: volume, then truncate, at 16 kHz
//   q 1  round            q 2  TPDF dither
//   q 3  TPDF dither + shaping, NTF (1 - z^-1)^2 (the default)
static uint8_t  sndOutQ = 3;
static float    sndOutVol = 0.0f;
static float    sndNsE1 = 0.0f, sndNsE2 = 0.0f;    // the shaper's last two errors
static uint32_t sndDith = 0x2545F491u;
// Silence stays silence: once the input has been exactly zero for longer than
// the FIR, the true output is exactly zero, so the dither stops too. On this
// hot amp the dither floor by itself was a hiss between sounds (2026-09-23);
// under sound it is masked, and it is what keeps quiet tails from turning to
// grit.
static int      sndOutZeros = 0;                   // input samples in a row that were exactly 0
static void sndOsReset() { memset(sndOsH, 0, sizeof(sndOsH)); sndOsP = 0; sndNsE1 = sndNsE2 = 0.0f; sndOutZeros = 0; }
static inline float sndRoundF(float y) { return (float)(int32_t)(y + (y >= 0.0f ? 0.5f : -0.5f)); }
static inline float sndTpdf() {                    // triangular, -1..1 step
  sndDith ^= sndDith << 13; sndDith ^= sndDith >> 17; sndDith ^= sndDith << 5;
  return ((float)(sndDith & 0xFFFFu) - (float)(sndDith >> 16)) * (1.0f / 65536.0f);
}
static inline int16_t sndWord16(float y, uint8_t q) {
  float u = (q >= 3) ? y - (2.0f * sndNsE1 - sndNsE2) : y;
  float r = sndRoundF(u + (q >= 2 ? sndTpdf() : 0.0f));
  if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
  if (q >= 3) { float e = r - u; sndNsE2 = sndNsE1; sndNsE1 = e > 4.0f ? 4.0f : (e < -4.0f ? -4.0f : e); }
  return (int16_t)r;
}

// in: one engine buffer (interleaved stereo, both channels the same). Fills
// sndOutBuf with frames * os bus frames; returns the bytes to write.
static size_t sndOutput(const int16_t* in, int frames, const SndI2sFmt& F, float volTgt) {
  const uint8_t L = F.os, q = sndOutQ;
  const int T = (L == 3) ? 16 : 20;
  const float* h = (L == 3) ? &SND_OS3[0][0] : &SND_OS2[0][0];
  float y[3];
  for (int i = 0; i < frames; i++) {
    sndOutVol += (volTgt - sndOutVol) * 0.00063f;          // ~100 ms, as the engine's own ramp was
    float v = (float)in[2 * i] * sndOutVol;
    if (q == 0) v = (float)(int32_t)v;
    if (L > 1) {
      sndOsP = (sndOsP == 0) ? T - 1 : sndOsP - 1;        // newest sample at the lowest index
      sndOsH[sndOsP] = sndOsH[sndOsP + T] = v;
      const float* x = sndOsH + sndOsP;                    // x[k] = the input k samples ago
      for (int p = 0; p < L; p++) {
        const float* c = h + p * T; float acc = 0.0f;
        for (int k = 0; k < T; k++) acc += c[k] * x[k];
        y[p] = acc;
      }
    } else y[0] = v;
    sndOutZeros = in[2 * i] ? 0 : sndOutZeros + 1;
    const bool hush = sndOutZeros > T;
    for (int p = 0; p < L; p++) {
      const int j = 2 * (i * L + p);
      sndOutBuf[j] = sndOutBuf[j + 1] = hush ? (int16_t)0 : sndWord16(y[p], q);
    }
    if (hush) sndNsE1 = sndNsE2 = 0.0f;
  }
  return (size_t)frames * L * 2 * sizeof(int16_t);
}

static bool sndI2sInit(uint8_t fmt = 0) {
  if (fmt >= SND_FMT_N) fmt = 0;
  const SndI2sFmt& F = SND_FMT[fmt];
  i2s_driver_uninstall(I2S_NUM_0);
  i2s_config_t c;
  memset(&c, 0, sizeof(c));
  c.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | (F.duplex ? I2S_MODE_RX : 0));
  c.sample_rate          = SND_SR * F.os;
  c.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format = F.comm;
  c.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL2;
  c.dma_buf_count        = F.bufs;
  c.dma_buf_len          = F.len;
  c.use_apll             = false;
  c.tx_desc_auto_clear   = true;          // an underrun plays silence, not the last buffer again
  c.fixed_mclk           = 0;
  c.mclk_multiple        = I2S_MCLK_MULTIPLE_DEFAULT;
  c.bits_per_chan        = I2S_BITS_PER_CHAN_16BIT;   // 32-bit slots play silence on this amp
  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num   = IIS_MCLK;           // K10 pin map (unihiker_k10.h)
  pins.bck_io_num   = IIS_BLCK;
  pins.ws_io_num    = IIS_LRCK;
  pins.data_out_num = IIS_DOUT;
  pins.data_in_num  = F.duplex ? IIS_DSIN : I2S_PIN_NO_CHANGE;
  if (i2s_driver_install(I2S_NUM_0, &c, 0, nullptr) != ESP_OK) { Log.error("SND: i2s_driver_install failed fmt=%s", F.name); return false; }
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) { Log.error("SND: i2s_set_pin failed fmt=%s", F.name); return false; }
  i2s_zero_dma_buffer(I2S_NUM_0);
  sndOsReset();
  sndDmaUs = (int64_t)F.bufs * F.len * 1000000 / (SND_SR * F.os);
  Log.notice("SND: I2S0 installed fmt=%d (%s) %u Hz, %ux%u DMA, live clk=%u",
             (int)fmt, F.name, (unsigned)(SND_SR * F.os), (unsigned)F.bufs, (unsigned)F.len, (unsigned)i2s_get_clk(I2S_NUM_0));
  return true;
}

// The K10 volume setting (0..9) as a gain after the engine's limiter. The
// K10's amp is very hot: on the first listen volume 1 at -22 dBFS was "really
// loud", and -40 dBFS is faint but clear. Tuned by ear on the sound desk
// (2026-09-23), the owner's comfortable middle is -37.7 dB, so that is 5
// ("this should be volume level 5") and 1-4 step down 3 dB each. Above it,
// 6-8 share the way up to 9 at sndVolMaxDb: 9 at -25.7 dB drove the amp into
// clipping on loud content (its rail, or the PC's USB sagging), so the top
// defaults to -31.7 dB and is a desk knob, set by ear just below the clip.
// Both ends are knobs (vmid, vmax); nothing the engine writes can exceed 9.
static float sndVolMidDb = -37.7f, sndVolMaxDb = -31.7f;
static float sndVolGain(uint8_t v) {
  if (v == 0) return 0.0f;
  if (v > 9) v = 9;
  const float mx = sndVolMaxDb > sndVolMidDb ? sndVolMaxDb : sndVolMidDb;
  return sndDb(v <= 5 ? sndVolMidDb - 3.0f * (float)(5 - v) : sndVolMidDb + (mx - sndVolMidDb) * (float)(v - 5) * 0.25f);
}

// ── Loopback capture: the board recording its own speaker ────────────────────
// The laptop's mic is useless for measuring (Windows' voice processing strips
// steady tones), so the K10 listens to itself through its onboard mics. In a
// duplex format the audio task drains the I2S RX DMA every pass; GET
// /sndtest?rec=1 arms a capture into PSRAM that starts on the same pass as the
// test sequence, and GET /sndrec.wav downloads it (scripts/sndsim/k10loop.py).
static int16_t* sndRecBuf = nullptr;         // PSRAM: 44-byte WAV header, then stereo frames
static constexpr uint32_t SND_REC_MAX = 30u * SND_SR;
static volatile uint32_t sndRecFrames = 0, sndRecTarget = 0;
static volatile uint8_t  sndRecState = 0;    // 0 idle, 1 recording, 2 done
static volatile float    sndRecReq = 0.0f;   // seconds, set by the HTTP handler
static void sndWavHeader(uint8_t* h, uint32_t frames) {
  uint32_t data = frames * 4, riff = 36 + data, sr = SND_SR, br = SND_SR * 4, fmtLen = 16;
  uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
  memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmtLen, 4);
  memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &sr, 4); memcpy(h + 28, &br, 4);
  memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
}
// ── Live knobs: GET /snddbg?key=value, and the sound desk (data/sound.html) ─
// Every knob lives in this table. The HTTP handler clamps a value, stores it
// and sets its dirty bit; the audio task applies it between renders
// (sndKnobApply), so nothing the engine owns is written from AsyncTCP. The
// volume and the music level are the game's own settings and are written
// straight through, as the settings message does. GET /snddbg?save=1 stores
// the set -- NVS namespace "sndmix", the volume and music level in "k10" --
// and sndStart() loads it at boot, so a tuned mix survives a reboot with no
// reflash; ?reset=1 restores the compiled mix (volume and music level stay)
// and forgets the saved one. style and tension only audition: never saved.
enum SndKnob : uint8_t {
  SNDK_VOL, SNDK_MLVL, SNDK_MUS, SNDK_SFX, SNDK_VGAIN, SNDK_DUCK, SNDK_REV, SNDK_NOISE, SNDK_WIND,
  SNDK_LP, SNDK_HP, SNDK_COMP, SNDK_Q, SNDK_VLP, SNDK_VUNV, SNDK_VSOFT, SNDK_VCHIRP, SNDK_DAC8,
  SNDK_LNARRATOR, SNDK_LCHANT, SNDK_LWHISPER, SNDK_LDOOM, SNDK_LBARKER, SNDK_LRADIO,   // SndSayStyle order
  SNDK_VMID, SNDK_VMAX,                                                                // the volume scale
  SNDK_STYLE, SNDK_TENSION, SNDK_COUNT
};
static_assert(SNDK_LRADIO - SNDK_LNARRATOR + 1 == SAY_STYLE_COUNT, "one level knob per speaking style");
struct SndKnobDef { const char* key; float mn, mx, step; };
static const SndKnobDef SND_KNOB[SNDK_COUNT] = {
  { "vol",      0,    9,     1     },   // the owner's volume; 0 = the engine sleeps
  { "mlvl",     0,    9,     1     },   // the K10 Music setting; 0 = no music or ambience
  { "mus",      0,    1.5f,  0.01f },   // the music bus at music level 9
  { "sfx",      0,    1.5f,  0.01f },   // the effect bus
  { "vgain",    0,    1.5f,  0.01f },   // the speech bus
  { "duck",     0,    0.9f,  0.01f },   // how far speech pushes the music down
  { "rev",      0,    2,     0.01f },   // reverb + echo returns
  { "noise",    0,    2,     0.01f },   // every patch's noise: breath, wind, hats
  { "wind",     0,    2,     0.01f },   // the wind texture
  { "lp",       0,    7500,  50    },   // master roll-off, Hz (0 = off)
  { "hp",       40,   600,   10    },   // master high-pass, Hz (4th order)
  { "comp",    -30,   0,     0.5f  },   // compressor threshold, dBFS (2:1 above it)
  { "q",        0,    3,     1     },   // the 16-bit cut (sndOutput): 0 truncate .. 3 dither + shaping
  { "vlp",      0,    7000,  50    },   // voice band limit, Hz (0 = off)
  { "vunv",     0,    2,     0.01f },   // voice hiss consonants
  { "vsoft",    0,    0.9f,  0.01f },   // voice source smoothing
  { "vchirp",  -1,    1,     1     },   // pitch pulse: -1 the chip's, 0 TMS5220, 1 TMS5100
  { "dac8",    -1,    1,     1     },   // 8-bit DAC grit: -1 per style, 0 off, 1 on
  { "lvl_narrator", 0, 1.5f, 0.01f },   // each speaking style's level, on top of vgain
  { "lvl_chant",    0, 1.5f, 0.01f },
  { "lvl_whisper",  0, 1.5f, 0.01f },
  { "lvl_doom",     0, 1.5f, 0.01f },
  { "lvl_barker",   0, 1.5f, 0.01f },
  { "lvl_radio",    0, 1.5f, 0.01f },
  { "vmid",   -60,  -20,   0.5f  },   // volume 5, dB: the owner's middle
  { "vmax",   -50,  -20,   0.5f  },   // volume 9, dB: the loudest, kept below where the amp clips
  { "style",   -1,    MS_COUNT - 1, 1 },  // audition a music style (-1 = the world decides)
  { "tension", -1,    1,     0.01f },   // pin the composer's tension (-1 = the world decides)
};
static float             sndKnobV[SNDK_COUNT];   // current values, as the desk shows them
static float             sndKnobD[SNDK_COUNT];   // the compiled defaults, captured at boot
static volatile uint32_t sndKnobDirty = 0;     // one bit per knob: set by HTTP, taken by the audio task
static bool              sndKnobSaved = false; // a saved set exists in NVS
static volatile int8_t   sndAmpReq = -1;       // GET /snddbg?amp= (debug: 1 mutes the speaker)
static int8_t sndAmpPin = 0;                 // the K10's eAmp_Gain expander pin (the library raises it to record)

static inline bool sndKnobSaves(int k) { return k >= SNDK_MUS && k != SNDK_STYLE && k != SNDK_TENSION; }
static float sndKnobClamp(int k, float v) {
  const SndKnobDef& d = SND_KNOB[k];
  if (!(v == v)) v = sndKnobD[k];                                   // NaN: the default
  if (d.step >= 1.0f) v = d.mn + roundf((v - d.mn) / d.step) * d.step;
  return v < d.mn ? d.mn : (v > d.mx ? d.mx : v);
}
static void sndKnobSet(int k, float v) {                            // any task
  v = sndKnobClamp(k, v);
  sndKnobV[k] = v;
  if (k == SNDK_VOL)       s_audioVol = (uint8_t)v;
  else if (k == SNDK_MLVL) s_musicVol = (uint8_t)v;                   // rides the next world snapshot (10 Hz)
  else __atomic_fetch_or(&sndKnobDirty, 1u << k, __ATOMIC_SEQ_CST);
}
// The audio task only (or setup(), before the task starts).
static void sndKnobApply(int k, float v) {
  switch (k) {
    case SNDK_MUS:     SM.musGain = v; break;                         // lands with the next world snapshot
    case SNDK_SFX:     SM.sfxGain = v; break;
    case SNDK_VGAIN:   SM.speechGain = v; break;
    case SNDK_DUCK:    SM.duckAmt = v; break;
    case SNDK_REV:     SC.fxMul = v; break;
    case SNDK_NOISE:   SC.noiseMul = v; break;
    case SNDK_WIND:    SMu.windMul = v; break;
    case SNDK_LP:      sndMasterSetLP(v); break;
    case SNDK_HP:      sndMasterSetHP(v); break;
    case SNDK_COMP:    SM.compThr = sndDb(v); break;
    case SNDK_Q:       sndOutQ = (uint8_t)v; sndNsE1 = sndNsE2 = 0.0f; break;
    case SNDK_VLP:     sndSpeechSetLP(v); break;
    case SNDK_VUNV:    SP.unvMul = v; break;                          // from the next phrase
    case SNDK_VSOFT:   SP.softMul = v; break;
    case SNDK_VCHIRP:  SP.chirpSel = (int8_t)v; break;
    case SNDK_DAC8:    SP.dac8Override = (int8_t)v; break;
    case SNDK_STYLE:   SMu.forceStyle = v < 0.0f ? 0xFF : (uint8_t)v; break;
    case SNDK_VMID:    sndVolMidDb = v; break;
    case SNDK_VMAX:    sndVolMaxDb = v; break;
    case SNDK_TENSION: SMu.forceTension = v; if (v >= 0.0f) SMu.tensionTgt = v; break;
    default:                                                        // the speaking styles, from their next phrase
      if (k >= SNDK_LNARRATOR && k <= SNDK_LRADIO) SP.styleLevel[k - SNDK_LNARRATOR] = v;
      break;
  }
}
// What the engine was built with, read back once after sndBegin().
static void sndKnobCapture() {
  const float v[SNDK_COUNT] = {
    (float)s_audioVol, (float)s_musicVol, SM.musGain, SM.sfxGain, SM.speechGain, SM.duckAmt,
    SC.fxMul, SC.noiseMul, SMu.windMul, SM.lpHz, SM.hpHz, 20.0f * log10f(SM.compThr), (float)sndOutQ,
    SP.vlpHz, SP.unvMul, SP.softMul, (float)SP.chirpSel, (float)SP.dac8Override,
    SP.styleLevel[0], SP.styleLevel[1], SP.styleLevel[2], SP.styleLevel[3], SP.styleLevel[4], SP.styleLevel[5],
    sndVolMidDb, sndVolMaxDb,
    -1.0f, -1.0f };
  for (int k = 0; k < SNDK_COUNT; k++) sndKnobV[k] = sndKnobD[k] = v[k];
}
static void sndKnobLoad() {                                         // setup(): the saved mix over the defaults
  Preferences p;
  if (!p.begin("sndmix", true)) return;                            // nothing saved yet
  sndKnobSaved = p.getUChar("v", 0) != 0;
  for (int k = 0; k < SNDK_COUNT; k++) {
    if (!sndKnobSaves(k)) continue;
    float v = p.getFloat(SND_KNOB[k].key, NAN);
    if (v == v) { sndKnobV[k] = sndKnobClamp(k, v); sndKnobApply(k, sndKnobV[k]); }
  }
  p.end();
}
static void sndKnobSave() {                                         // HTTP task; NVS is thread-safe
  Preferences p;
  if (p.begin("sndmix", false)) {
    for (int k = 0; k < SNDK_COUNT; k++) if (sndKnobSaves(k)) p.putFloat(SND_KNOB[k].key, sndKnobV[k]);
    p.putUChar("v", 1);
    p.end();
    sndKnobSaved = true;
  }
  saveK10Prefs();                                                   // the volume and the music level
}
static void sndKnobReset() {
  for (int k = 0; k < SNDK_COUNT; k++) if (k >= SNDK_MUS) sndKnobSet(k, sndKnobD[k]);
  Preferences p;
  if (p.begin("sndmix", false)) { p.clear(); p.end(); }
  sndKnobSaved = false;
}
// ── The desk's JSON (GET /snddbg and /sndinfo, game-server.hpp) ─────────────
static void sndJsonStr(String& j, const char* s) {
  j += '"';
  for (; s && *s; s++) {
    if (*s == '"' || *s == '\\') { j += '\\'; j += *s; }
    else if ((uint8_t)*s < 0x20) j += ' ';
    else j += *s;
  }
  j += '"';
}
static void sndJsonNum(String& j, float v) { char b[20]; snprintf(b, sizeof(b), "%g", (double)v); j += b; }
static String sndKnobJson() {
  String j; j.reserve(480);
  j += "{\"k\":{";
  for (int k = 0; k < SNDK_COUNT; k++) {
    if (k) j += ',';
    sndJsonStr(j, SND_KNOB[k].key); j += ':';
    sndJsonNum(j, k == SNDK_VOL ? (float)s_audioVol : k == SNDK_MLVL ? (float)s_musicVol : sndKnobV[k]);
  }
  j += "},\"saved\":"; j += sndKnobSaved ? "true" : "false";
  j += ",\"playing\":"; sndJsonStr(j, MUS_STYLE_NAME[SMu.style < MS_COUNT ? SMu.style : 0]);
  j += ",\"fmt\":"; sndJsonStr(j, SND_FMT[sndFmt].name);
  j += '}';
  return j;
}
static String sndInfoJson() {
  String j; j.reserve(6144);
  j += "{\"knobs\":[";
  for (int k = 0; k < SNDK_COUNT; k++) {
    const SndKnobDef& d = SND_KNOB[k];
    if (k) j += ',';
    j += "{\"k\":"; sndJsonStr(j, d.key);
    j += ",\"min\":"; sndJsonNum(j, d.mn);
    j += ",\"max\":"; sndJsonNum(j, d.mx);
    j += ",\"step\":"; sndJsonNum(j, d.step);
    j += ",\"def\":"; sndJsonNum(j, sndKnobD[k]);
    j += '}';
  }
  j += "],\"sfx\":[";
  for (int i = 0; i < SFX_COUNT; i++) { if (i) j += ','; sndJsonStr(j, SFX_NAME[i]); }
  j += "],\"say\":[";
  for (int i = 0; i < VOC_COUNT; i++) { if (i) j += ','; sndJsonStr(j, SND_VOCAB[i].text); }
  j += "],\"voices\":[";
  for (int i = 0; i < SAY_STYLE_COUNT; i++) { if (i) j += ','; sndJsonStr(j, SAY_STYLE_NAME[i]); }
  j += "],\"music\":[";
  for (int i = 0; i < MS_COUNT; i++) { if (i) j += ','; sndJsonStr(j, MUS_STYLE_NAME[i]); }
  j += "],\"story\":[";
  for (int i = 0; i < SS_COUNT; i++) { if (i) j += ','; sndJsonStr(j, SND_STORY_NAME[i]); }
  j += "],\"state\":"; j += sndKnobJson();
  j += "}\n";
  return j;
}
static volatile float sndRxRms = 0.0f, sndRxDc = 0.0f;   // the mics' last block, for GET /snddbg

static void sndTaskFn(void*) {
  static int16_t buf[2 * SND_IO_FRAMES];
  static int16_t rx[2 * SND_IO_FRAMES];
  int64_t audioUs = esp_timer_get_time();             // where the DMA queue should be
  for (;;) {
    if (sndFmtReq) {                                  // a format A/B request (GET /sndtest?fmt=N)
      uint8_t f = (uint8_t)(sndFmtReq - 1); sndFmtReq = 0;
      if (f < SND_FMT_N) {
        if (sndI2sInit(f)) sndFmt = f;
        else if (!sndI2sInit(sndFmt)) Log.error("SND: fmt %u failed and fmt %u did not come back", (unsigned)f, (unsigned)sndFmt);
      }
      audioUs = esp_timer_get_time();
    }
    uint32_t dirty = __atomic_exchange_n(&sndKnobDirty, 0u, __ATOMIC_SEQ_CST);
    for (int k = 0; dirty; k++, dirty >>= 1) if (dirty & 1u) sndKnobApply(k, sndKnobV[k]);
    if (sndAmpReq >= 0) { sndAmpPin = sndAmpReq; sndAmpReq = -1; digital_write(eAmp_Gain, (uint8_t)sndAmpPin); }
    if (sndRecReq > 0.0f) {                           // arm a capture (GET /sndtest?rec=1)
      float secs = sndRecReq; sndRecReq = 0.0f;
      if (!sndRecBuf) sndRecBuf = (int16_t*)heap_caps_malloc(44 + SND_REC_MAX * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (sndRecBuf && SND_FMT[sndFmt].duplex) {
        size_t got = 1;
        while (got) { got = 0; i2s_read(I2S_NUM_0, rx, sizeof(rx), &got, 0); }   // drop stale RX
        sndRecFrames = 0;
        sndRecTarget = (uint32_t)(secs * SND_SR); if (sndRecTarget > SND_REC_MAX) sndRecTarget = SND_REC_MAX;
        sndRecState = 1;
      } else {
        Log.error("SND: capture unavailable (buf=%s, fmt=%s)", sndRecBuf ? "ok" : "none", SND_FMT[sndFmt].name);
      }
    }
    if (s_audioVol == 0) {
      // Muted: don't spend the CPU. Drop what queued up so unmuting does not
      // replay a backlog; the DMA auto-clears to silence on its own.
      SND_LOCK(); sndQTail = sndQHead; SP.qn = 0; SND_UNLOCK();
      vTaskDelay(pdMS_TO_TICKS(50));
      audioUs = esp_timer_get_time();
      continue;
    }
    int64_t t0 = esp_timer_get_time();
    sndRender(buf, SND_IO_FRAMES);
    size_t bytes = sndOutput(buf, SND_IO_FRAMES, SND_FMT[sndFmt], sndVolGain(s_audioVol));
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    sndStUs += us; sndStN++; if (us > sndStMaxUs) sndStMaxUs = us;
    // Late = we got back to the DMA after it had already run dry. With 96 ms
    // of buffer that takes a stall of ~80 ms: count it, it is the underrun.
    int64_t now = esp_timer_get_time();
    audioUs += (int64_t)SND_IO_FRAMES * 1000000 / SND_SR;
    if (now > audioUs + 10000) { sndStLate++; audioUs = now; }
    if (audioUs < now - sndDmaUs) audioUs = now - sndDmaUs;
    size_t w = 0;
    i2s_write(I2S_NUM_0, sndOutBuf, bytes, &w, portMAX_DELAY);
    if (SND_FMT[sndFmt].duplex) {                     // drain the mics every pass
      size_t got = 1;
      while (got) {
        got = 0;
        i2s_read(I2S_NUM_0, rx, sizeof(rx), &got, 0);
        if (got) {                                    // level of what the mics deliver, for /snddbg
          uint32_t n = (uint32_t)(got / 2); float s2 = 0.0f, s1 = 0.0f;
          for (uint32_t i = 0; i < n; i++) { float v = rx[i] * (1.0f / 32768.0f); s1 += v; s2 += v * v; }
          sndRxRms = sqrtf(s2 / n); sndRxDc = s1 / n;
        }
        if (got && sndRecState == 1) {
          uint32_t n = (uint32_t)(got / 4);
          if (sndRecFrames + n > sndRecTarget) n = sndRecTarget - sndRecFrames;
          memcpy(sndRecBuf + 22 + sndRecFrames * 2, rx, n * 4);
          sndRecFrames += n;
          if (sndRecFrames >= sndRecTarget) { sndWavHeader((uint8_t*)sndRecBuf, sndRecFrames); sndRecState = 2; }
        }
      }
    }
  }
}

// Called once from setup(), after k10.begin() (which installs I2S the first time).
static void sndStart() {
  if (!sndBegin(esp_random())) { Log.error("SND: engine init failed (out of memory?) -- silent"); return; }
  SM.vol = SM.volTgt = 1.0f;          // full scale out of the engine: sndOutput() applies the owner's volume
  sndKnobCapture();                   // the compiled mix, for ?reset=1
  sndKnobLoad();                      // then whatever the sound desk saved
  if (!sndI2sInit(sndFmt)) {
    Log.error("SND: I2S fmt %s failed -- falling back to %s", SND_FMT[sndFmt].name, SND_FMT[SND_FMT_SAFE].name);
    sndFmt = SND_FMT_SAFE;
    if (!sndI2sInit(sndFmt)) { Log.error("SND: I2S reinstall failed -- silent"); return; }
  }
  // Core 0 (Wi-Fi's core, which runs lighter than the LCD's) above AsyncTCP
  // (10) and below lwIP (18) and the Wi-Fi task (23): the speaker is never
  // starved by a web request, and the radio is never starved by the speaker.
  BaseType_t ok = xTaskCreatePinnedToCore(sndTaskFn, "snd", 6144, nullptr, 11, &sndTaskH, 0);
  sndOk = (ok == pdPASS);
  Log.notice("SND: engine %s, %d voices, internal heap=%uKB psram=%uKB",
             sndOk ? "running" : "TASK FAILED", SND_VOICES,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(ESP.getFreePsram() / 1024));
}

// ── k10Play: the old entry point ─────────────────────────────────────────────

static void k10PlaySeq(const ToneStep* seq, const char* name = nullptr) {
  if (!sndOk || s_audioVol == 0 || !seq) return;
  (void)name;
  // The Doom's ostinato is the composer's business while music is on; the
  // engine plays the old figure itself when it is not (SS_DOOM_BAND).
  if (seq == MOTIF_DOOM_FAR)  { sndStory(SS_DOOM_BAND, 1); return; }
  if (seq == MOTIF_DOOM_NEAR) { sndStory(SS_DOOM_BAND, 2); return; }
  if (seq == MOTIF_DOOM_HUNT) { sndStory(SS_DOOM_BAND, 3); return; }
  if (seq == MOTIF_DOOM_LOST) { sndStory(SS_DOOM_BAND, 0); return; }
  // Damage sites whose event (drainEvents) carries the story sound: silent here.
  if (seq == MOTIF_WARNING_GRUNT || seq == MOTIF_BUNKER_ALARM || seq == MOTIF_SEWER_ECHO) return;
  if (seq == MOTIF_DISTANT_THUD) { sndStory(SS_QUAKE); return; }   // broadcastQuake()
  if (seq == MOTIF_GEIGER)       { sndStory(SS_GEIGER); return; }
  struct Map { const ToneStep* s; uint8_t id; };
  static const Map MAP[] = {
    { MOTIF_DARK_ENTRY, SFX_DARK_ENTRY }, { MOTIF_DARK_DEPART, SFX_DARK_DEPART },
    { MOTIF_GROSS_SLUDGE, SFX_SLUDGE },   { MOTIF_BROKEN_TECH, SFX_BROKEN_TECH },
    { MOTIF_HEAVY_DOOR_DRAG, SFX_DOOR_DRAG }, { MOTIF_MUTANT_BREATH, SFX_MUTANT_BREATH },
    { MOTIF_POWER_DOWN, SFX_POWER_DOWN }, { MOTIF_ACID_DRIP, SFX_ACID_DRIP },
    { MOTIF_CREEPING_RUST, SFX_CREEPING_RUST }, { MOTIF_SYSTEM_FAULT, SFX_SYSTEM_FAULT },
    { MOTIF_DEAD_BATTERY, SFX_DEAD_BATTERY }, { MOTIF_ROTTEN_CHORD, SFX_ROTTEN_CHORD },
    { MOTIF_RADIO_BLIP, SFX_RADIO_BLIP }, { MOTIF_WEIRD_ANOMALY, SFX_WEIRD_ANOMALY },
    { MOTIF_SCREEN_CLICK, SFX_CLICK },    { SEQ_SCORE_UP, SFX_SCORE_UP },
  };
  for (const Map& m : MAP) if (m.s == seq) { sndStory(SS_SFX, m.id); return; }
  // Anything newer than this table still plays, on the music box.
  sndTonesReq = seq;
}
#define k10Play(seq) k10PlaySeq(seq, #seq)

// Dawn arrives once per survivor in drainEvents(); the day is announced once.
static uint16_t sndLastDawn = 0xFFFF;
static void sndDawn(uint16_t day) {
  if (day == sndLastDawn) return;
  sndLastDawn = day;
  sndStory(SS_DAWN, (uint8_t)(day & 0xFF), (uint8_t)(day >> 8));
}

// ── The world snapshot, published at 10 Hz from loop() ───────────────────────
static float sndPrevDayFrac = 0.0f;
static uint32_t sndLogMs = 0;

static void sndPublishWorld(uint32_t snapDayTick, uint8_t snapWeather, uint16_t snapDay,
                            uint8_t slotMask, uint8_t aliveMask) {
  if (!sndOk) return;
  DreadSnapshot d = dreadRead();
  SndWorld w; memset(&w, 0, sizeof(w));
  w.slotMask = slotMask; w.aliveMask = aliveMask;
  w.dayFrac  = (float)snapDayTick / (float)DAY_TICKS; if (w.dayFrac >= 1.0f) w.dayFrac = 0.999f;
  w.day      = snapDay;
  w.weather  = snapWeather < 6 ? snapWeather : 0;
  w.tcLevel  = d.tcLevel;  w.tcWeight = d.tcWeight;
  w.doomClose = d.doomClose; w.doomAware = d.doomAware;
  w.attrition = d.attrition; w.hunger = d.hunger; w.thirst = d.thirst;
  w.radLoad = d.radLoad; w.woundLoad = d.woundLoad; w.fireClose = d.fireClose;
  w.under = d.under; w.connected = d.connected; w.allUnder = d.allUnder; w.encActive = d.encActive;
  w.caravanNear = g_sndCaravanNear;
  w.musicLevel = s_musicVol;
  sndSetWorld(w);
  // Nightfall has no event of its own: the day clock crossing into the dark is it.
  if (d.connected && sndPrevDayFrac < 0.78f && w.dayFrac >= 0.78f) sndStory(SS_DUSK);
  sndPrevDayFrac = w.dayFrac;

  uint32_t now = millis();
  if (now - sndLogMs >= 30000) {
    sndLogMs = now;
    uint32_t n = sndStN ? sndStN : 1;
    // Budget: SND_IO_FRAMES at 16 kHz is 16000 us per render.
    Log.notice("SND: render avg=%uus max=%uus of 16000us (%u%% cpu) voices max=%u late=%u style=%s tension=%u%% stack=%u",
               (unsigned)(sndStUs / n), (unsigned)sndStMaxUs, (unsigned)(sndStUs / n / 160),
               (unsigned)SST.maxVoices, (unsigned)sndStLate, MUS_STYLE_NAME[SMu.style],
               (unsigned)(SMu.tension * 100), (unsigned)uxTaskGetStackHighWaterMark(sndTaskH));
    sndStUs = 0; sndStN = 0; sndStMaxUs = 0; SST.maxVoices = 0;
  }
}

// ── Score milestones, the threat clock, and the world snapshot ───────────────
static void checkScoreAudio() {
  uint32_t teamScore = 0;
  uint8_t  snapTC    = 0;
  uint32_t snapDayTick = 0; uint8_t snapWeather = 0; uint16_t snapDay = 0;
  uint8_t  slotMask = 0, aliveMask = 0;
  bool     got = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!G.players[i].connected) continue;
      teamScore += G.players[i].score;
      slotMask |= (uint8_t)(1u << i);
      if (G.players[i].ll > 0) aliveMask |= (uint8_t)(1u << i);
    }
    snapTC      = G.threatClock;
    snapDayTick = G.dayTick;
    snapWeather = G.weatherPhase;
    snapDay     = (uint16_t)G.dayCount;
    xSemaphoreGive(G.mutex);
    got = true;
  }
  if (!got) return;
  sndPublishWorld(snapDayTick, snapWeather, snapDay, slotMask, aliveMask);

  if (teamScore / 100 != k10TeamScore / 100) {
    if (teamScore > k10TeamScore) {
      sndStory(SS_SCORE_UP);
      // Good news opens outward from the middle lamp.
      ledCue(0x28, 0xE0, 0x58, CUE_BLOOM, SPAN_ALL, 700, CUEP_INFO, 1);
    } else {
      sndStory(SS_SCORE_DOWN);
      // Bad news is a double throb, not a bloom.
      ledCue(0xC8, 0x18, 0x18, CUE_PULSE, SPAN_ALL, 800, CUEP_INFO, 2);
    }
  }
  k10TeamScore = teamScore;

  uint8_t tcLvl = (snapTC >= TC_THRESHOLD_D) ? 4 :
                  (snapTC >= TC_THRESHOLD_C) ? 3 :
                  (snapTC >= TC_THRESHOLD_B) ? 2 :
                  (snapTC >= TC_THRESHOLD_A) ? 1 : 0;
  if (tcLvl > k10PrevTCLevel) {
    // The bell tolls the band reached, the chronicler reads the line, and at
    // the last band the old air-raid siren goes up (snd-engine.hpp SS_THREAT).
    sndStory(SS_THREAT, tcLvl);
    // The LCD gives it a panel: hazard tape and the band reached, or at the
    // last band TOO LATE in wet ink (ui-fx.hpp).
    static const char* const TC_LINE[5] = {
      "",
      "The clock turns. Something out there woke up.",
      "The clock turns again. The waste is paying attention.",
      "The clock turns. Nothing out here is sleeping now.",
      "The clock has run out of patience with us.",
    };
    fxCue(FXK_THREAT, -1, TC_LINE[tcLvl], nullptr, tcLvl);
    // The clock crossing a threshold is a hard, countable signal, so it blinks
    // once per band reached rather than fading in like a hazard. applyDread()
    // then carries the new band continuously; this is just the announcement.
    // Level 4 gets the full alarm treatment across the whole strip.
    if (tcLvl == 4)
      ledCue(255, 40, 30, CUE_BLINK, SPAN_ALL, 1400, CUEP_ALARM, 5);
    else
      ledCue(210, 70, 40, CUE_BLINK, SPAN_INWARD, 900, CUEP_INFO, tcLvl);
  }
  k10PrevTCLevel = tcLvl;
}
