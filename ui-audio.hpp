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

  for (const ToneStep* s = (const ToneStep*)arg; s->freq != 0; s++) {
    int ms   = (s->freq < 0) ? -(s->freq) : s->beat;
    int freq = (s->freq < 0) ? 0          : s->freq;
    int n    = ms * 8;
    for (int i = 0; i < n; i++) {
      float   amp = 32767.0f * (s_audioVol / 5.0f);
      int16_t v = freq ? (int16_t)(amp * sinf(i * (float)TWO_PI * freq / 8000.0f)) : 0;
      int16_t buf[2] = {v, v};
      i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
    }
  }
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
  bool     crisis    = false;
  if (xSemaphoreTake(G.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (G.players[i].connected) teamScore += G.players[i].score;
    snapTC  = G.threatClock;
    crisis  = G.crisisState;
    xSemaphoreGive(G.mutex);
  }

  if (teamScore / 100 != k10TeamScore / 100) {
    if (teamScore > k10TeamScore) {
      k10Play(SEQ_SCORE_UP);
      k10LedPulse = millis() + 600;
      k10PulseR = 0x00; k10PulseG = 0xC8; k10PulseB = 0x30;
    } else {
      k10Play(MOTIF_ROTTEN_CHORD);
      k10LedPulse = millis() + 600;
      k10PulseR = 0xC8; k10PulseG = 0x10; k10PulseB = 0x10;
    }
  }
  k10TeamScore = teamScore;

  uint8_t tcLvl = (snapTC >= TC_THRESHOLD_D) ? 4 :
                  (snapTC >= TC_THRESHOLD_C) ? 3 :
                  (snapTC >= TC_THRESHOLD_B) ? 2 :
                  (snapTC >= TC_THRESHOLD_A) ? 1 : 0;
  if (tcLvl > k10PrevTCLevel) {
    if (tcLvl == 4 || crisis) k10Play(MOTIF_BUNKER_ALARM); else k10Play(MOTIF_WARNING_GRUNT);
  }
  k10PrevTCLevel = tcLvl;
}
