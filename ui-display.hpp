#pragma once
// ── ui-display.hpp ──────────────────────────────────────────────────────────
// Umbrella include for the K10 display subsystem. Include this file only.

#include "ui-helpers.hpp"
#include "ui-fx.hpp"        // LCD compositor: cut-ins, shake, CRT failure; before anything that cues it
#include "ui-boot.hpp"
#include "snd-engine.hpp"   // the sound engine (platform-neutral): before anything that raises sndStory()
#include "ui-leds.hpp"
#include "ui-audio.hpp"
#include "ui-screens.hpp"
#include "ui-upload.hpp"
#include "ui-death.hpp"
