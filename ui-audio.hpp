#pragma once
// ── ui-audio.hpp ─────────────────────────────────────────────────────────────
// Non-blocking I2S tone sequencer and game-event audio alerts (score, TC).

static TaskHandle_t  s_toneTask = nullptr;
static const char*   s_toneName = nullptr;

static void toneTaskFn(void* arg) {
  // playTone(freq, beat) treats 'beat' as sample count at 8000 Hz, not ms.
  // It also calls i2s_zero_dma_buffer() immediately after writing, which
  // cancels queued audio before the DMA clock has time to play it.
  // Write directly to I2S instead: beat_ms * 8 = samples at 8 kHz.
  // i2s_write(portMAX_DELAY) throttles to DMA playback speed naturally.
  size_t written;
  uint32_t savedRate = i2s_get_clk(I2S_NUM_0);
  i2s_set_sample_rates(I2S_NUM_0, 8000);
  uint32_t liveRate  = i2s_get_clk(I2S_NUM_0);  // what the peripheral actually took
  uint32_t toneT0    = millis();
  int      nominalMs = 0;

  for (const ToneStep* s = (const ToneStep*)arg; s->freq != 0; s++) {
    int ms   = (s->freq < 0) ? -(s->freq) : s->beat;
    int freq = (s->freq < 0) ? 0          : s->freq;
    int n    = ms * 8;
    nominalMs += ms;
    for (int i = 0; i < n; i++) {
      float   amp = 32767.0f * (s_audioVol / 5.0f);
      int16_t v = freq ? (int16_t)(amp * sinf(i * (float)TWO_PI * freq / 8000.0f)) : 0;
      int16_t buf[2] = {v, v};
      i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
    }
  }
  // Nominal vs measured is the decisive read on whether i2s_set_sample_rates()
  // actually took: if 'live' is not 8000, every duration in tone-motifs.hpp is
  // scaled by 8000/live and every pitch is shifted by the same factor.
  Log.notice("TONE %s rate saved=%u live=%u nominal=%dms actual=%dms",
             s_toneName ? s_toneName : "?", (unsigned)savedRate, (unsigned)liveRate,
             nominalMs, (int)(millis() - toneT0));
  // DMA buffer = 3*300 samples @ 8kHz = 112.5 ms max latency; drain before zeroing.
  vTaskDelay(pdMS_TO_TICKS(115));
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_set_sample_rates(I2S_NUM_0, savedRate);
  s_toneTask = nullptr;
  s_toneName = nullptr;
  vTaskDelete(nullptr);
}

static void k10PlaySeq(const ToneStep* seq, const char* name = nullptr) {
  if (s_audioVol == 0) {
    return;
  }
  if (s_toneTask != nullptr) {
    return;
  }
  s_toneName = name;
  xTaskCreate(toneTaskFn, "tone", 4096, (void*)seq, 1, &s_toneTask);
}

// Stringifies the motif/sequence name automatically
#define k10Play(seq) k10PlaySeq(seq, #seq)

// ── Score milestones, TC thresholds, and crisis audio alerts ─────────────────
static void checkScoreAudio() {
  uint32_t teamScore = 0;
  uint8_t  snapTC    = 0;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (G.players[i].connected) teamScore += G.players[i].score;
    snapTC  = G.threatClock;
    xSemaphoreGive(G.mutex);
  }

  if (teamScore / 100 != k10TeamScore / 100) {
    if (teamScore > k10TeamScore) {
      k10Play(SEQ_SCORE_UP);
      // Good news opens outward from the middle lamp.
      ledCue(0x28, 0xE0, 0x58, CUE_BLOOM, SPAN_ALL, 700, CUEP_INFO, 1);
    } else {
      k10Play(MOTIF_ROTTEN_CHORD);
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
    if (tcLvl == 4) k10Play(MOTIF_BUNKER_ALARM); else k10Play(MOTIF_WARNING_GRUNT);
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
