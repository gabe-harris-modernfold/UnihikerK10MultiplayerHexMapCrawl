#pragma once
// -- ui-death.hpp ------------------------------------------------------------
// The death screen: when a survivor goes down, the LCD drops whatever it was
// showing and gives the whole 240x320 to a burning skull.
//
// Same division of labour as ui-upload.hpp -- drainEvents() (Core 1) only
// writes state here, never the canvas; the LCD refresh path in the main loop
// checks DeathUI::isActive() and calls drawDeathScreen(). The window is
// DEATH_HOLD_MS long and repaints fast so the flames move.
//
// The art is 25 rows of 46 columns. At the 6 px glyph pitch that would be
// 276 px -- wider than the panel -- so it is drawn a character at a time on a
// 5 px advance. The 5x7 glyphs are 5 px wide, so they sit flush instead of
// overlapping, and the rules and diagonals join up into continuous strokes.
//
// The art table is one flat string; all of the colour is decided at draw time,
// by four things:
//   * distance from the bone, which sets how hot a flame glyph is. The skull
//     is what is burning, so the plume is at its brightest where it touches
//     bone and cools on the way out -- not the other way round.
//   * which part of the skull a bone glyph belongs to, so the cranium, the
//     face and the jaw are lit as three separate planes instead of one flat
//     white, with the outermost columns warmed by the fire beside them.
//   * a per-glyph hash of the frame, so the flames shimmer one character at a
//     time rather than a whole row at once.
//   * coals painted under the eye sockets, so the hollows read as hollows with
//     something alive in them rather than as outlines on a flat face.

namespace DeathUI {
  static volatile bool     _active = false;
  static volatile uint32_t _sinceMs = 0;
  static char              _who[16]  = {0};
  static char              _line[48] = {0};

  static constexpr uint32_t DEATH_HOLD_MS = 6000;

  inline void begin(const char* who, const char* line) {
    strlcpy(_who,  who  ? who  : "Someone", sizeof(_who));
    strlcpy(_line, line ? line : "",        sizeof(_line));
    _sinceMs = millis();
    _active  = true;
  }

  inline bool isActive() {
    if (!_active) return false;
    if (millis() - _sinceMs > DEATH_HOLD_MS) { _active = false; return false; }
    return true;
  }
}

static constexpr int DEATH_COLS  = 46;
static constexpr int DEATH_ROWS  = 25;
static constexpr int DEATH_ADV   = 5;   // px per column (glyphs are 5 px wide)
static constexpr int DEATH_PITCH = 7;   // px per row (glyphs are 7 px tall)

static const char* const DEATH_ART[DEATH_ROWS] = {
  "                                (             ",
  "        .                )            )       ",
  "              (   (|                        . ",
  "          )   )\\/ ( ( (                       ",
  "   *  (   ((  /     ))\\))  (  )    )          ",
  "  (     \\   )\\(          |  ))( )  (|         ",
  " >)     ))/   |          )/  \\((  ) \\         ",
  " (     (      .        -.     V )/   )(    (  ",
  "  \\   /     .   \\            .       \\))   )) ",
  "    )(      (  | |   )            .    (  /   ",
  "    )(    ,'))     \\ /          \\( `.    )    ",
  "   (\\>  ,'/__      ))            __`.  /      ",
  "  ( \\   | /    ___   ( \\/   ___   \\ | ( (     ",
  "   \\.)  |/    /   \\__    __/   \\   \\|  ))     ",
  " .  \\. |>     \\     | __ |     /   <|  /      ",
  "        )/    \\____/ :..: \\____/    \\ <       ",
  "   )   \\ (|__  .      / ;: \\          __| )  (",
  "  ((    )\\)  ~--_    --  --     _--~    /  )) ",
  "   \\    (    |  ||           ||  |    (  /    ",
  "      \\.  |  ||_           _||  |   /         ",
  "        > :  |  ~V+-I_I_I-+V~  |  : (.        ",
  "       (  \\:  T\\   _     _   /T  : ./         ",
  "        \\  :    T^T T-+-T T^T    ;<           ",
  "         \\..`_     -+-       _'  )      )     ",
  "          . `--=.._____..=--'. ./             ",
};

// Columns that belong to the skull on each row; everything outside is fire.
// A lo greater than hi means the whole row is fire.
//
// These hug the bone tightly on purpose. A flame glyph caught inside the band
// gets painted as bone, and a white tick floating out in the plume is the one
// mistake that reads instantly -- so the sides of the cranium, the jaw corners
// and the whole of rows 9-10 (which are plume rising over the crown, not
// skull) are all left to the fire.
struct DeathBand { uint8_t lo, hi; };
static const DeathBand DEATH_BAND[DEATH_ROWS] = {
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  { 99,  0 },
  {  8, 36 },
  {  8, 37 },
  {  8, 37 },
  {  7, 36 },
  {  8, 38 },
  {  9, 40 },
  {  8, 38 },
  {  8, 38 },
  { 10, 36 },
  { 10, 34 },
  { 10, 33 },
  { 11, 34 },
  {  9, 30 },
  { 10, 29 },
};

// First row that is skull rather than plume. Rows above it have no band, so
// the heat falloff measures down to this row's band instead.
static constexpr int DEATH_CROWN = 11;

// The fire ramp, hot core to cold smoke. Steps are >= 8 in red and blue so
// none of them collapse into each other once c16() drops the screen to RGB565,
// and the tail stays dark enough to read as unlit against the panel.
//
// The hot end deliberately stops at a saturated amber rather than going white.
// Bone is the only near-white on this screen, and flame that reaches white
// right where it touches the skull stops reading as flame at all -- the halo
// just looks like more skull. Tier 0 is a shade paler than tier 1 and is kept
// for embers, which are the only marks allowed to out-burn the plume.
static constexpr int DEATH_FIRE_N = 14;
static const uint32_t DEATH_FIRE[DEATH_FIRE_N] = {
  0xFFEEA8, 0xFFD86E, 0xFFC24C, 0xFCAB34, 0xF59426, 0xEA7D1A,
  0xDC6714, 0xCB5410, 0xB8430D, 0xA3340B, 0x8C2709, 0x741D07,
  0x5C1405, 0x460E03,
};

// Bone is lit as three planes: the cranium takes the most light, the face is
// a stop under it, the jaw sits in the skull's own shadow down over the coals.
static constexpr int DEATH_ZONE_FACE = 14;   // first face row
static constexpr int DEATH_ZONE_JAW  = 20;   // first jaw row
static const uint32_t DEATH_BONE[3] = { 0xF4EEE0, 0xDCD2BE, 0xBEB094 };

// Bone within DEATH_RIM columns of the fire is lit by the fire, not by the
// scene, so it goes warm. It flickers between two tones on the frame hash --
// the same beat as the flames, so the firelight moves across the skull.
static constexpr int DEATH_RIM = 2;
static const uint32_t DEATH_RIM_HI[3] = { 0xFFE0AC, 0xF0C084, 0xD09A5C };
static const uint32_t DEATH_RIM_LO[3] = { 0xE6C68C, 0xCEA264, 0xA6763C };

// The hollows, in art cells. Bone within one cell of any of them is lit from
// inside rather than by the scene. The two with coal = true are also painted
// out before the art goes down, so the socket outlines end up in front of the
// glow; the nasal aperture is only five glyphs across and anything painted
// into it reads as a box floating next to the nose, so it lights its
// surroundings without being filled.
//
// A filled hollow goes down as three bands with the top and bottom pulled in,
// because a plain rectangle shows its corners through the hexagon the art
// draws around it.
struct DeathSocket { uint8_t c0, c1, r0, r1; bool coal; };
static const DeathSocket DEATH_SOCKET[3] = {
  { 15, 19, 13, 16, true  },   // left eye, inside the \____/ floor at 14..19
  { 27, 31, 13, 16, true  },   // right eye, inside the \____/ floor at 26..31
  { 22, 27, 15, 18, false },   // nasal aperture -- lights the bone, unfilled
};
// A socket is mostly dark -- a hollow with something small still alight at the
// bottom of it. Filling one with a broad ember red instead reads as a red
// sticker pasted over the skull, so the hollow tone sits barely off black and
// only the coal in the middle of it is bright.
static constexpr uint32_t DEATH_HOLLOW  = 0x1C0704;
static const uint32_t DEATH_COAL_MID[3] = { 0x4A1206, 0x5C1707, 0x701C08 };
static const uint32_t DEATH_COAL_HOT[3] = { 0x9E300C, 0xB83C0E, 0xD24A12 };

// One glyph at an arbitrary x -- canvasText8() advances 6 px per character and
// the art needs 5, so the art is set a character at a time.
static inline void canvasChar(char c, int x, int y, uint32_t col) {
  char s[2] = { c, 0 };
  canvasText8(s, x, y, col);
}

// Distance from a plume glyph to the nearest bone, in art cells, measured
// against every skull row rather than just its own -- fire beside the jaw is
// close to the cheek above it, and measuring only its own row would call it
// cold. This is what sets the temperature: the skull is what is burning, so
// the plume is at its brightest where it touches bone and cools all the way
// out to smoke. Tier 0 is left to the embers.
static inline int deathHeat(int r, int c) {
  int best = 99;
  for (int sr = DEATH_CROWN; sr < DEATH_ROWS; sr++) {
    const DeathBand& b = DEATH_BAND[sr];
    int h = (c < b.lo) ? (b.lo - c) : (c > b.hi ? c - b.hi : 0);
    int v = (r < sr) ? (sr - r) : (r - sr);
    if (h + v < best) best = h + v;
  }
  return 1 + best;
}

// Bone next to a hollow is lit by what is in the hollow, not by the scene.
static inline bool deathSocketLit(int r, int c) {
  for (int i = 0; i < 3; i++) {
    const DeathSocket& s = DEATH_SOCKET[i];
    if (c + 1 >= s.c0 && c <= s.c1 && r + 1 >= s.r0 && r <= s.r1) return true;
  }
  return false;
}

// Loose marks in the plume -- specks and ticks rather than strokes -- are
// embers rather than flame, and read as embers only if they are hotter than
// the flame around them.
static inline bool deathIsEmber(char ch) {
  return ch == '.' || ch == ',' || ch == '\'' || ch == '*' || ch == '`';
}

static void drawDeathScreen() {
  canvas.fillScreen(0x0000);

  // The fire breathes: every ~110 ms the whole plume steps along the ramp, and
  // on top of that each glyph carries its own offset, so the flames flicker
  // per character instead of a row at a time.
  uint32_t phase = millis() / 110;

  const int x0 = (240 - DEATH_COLS * DEATH_ADV) / 2;
  const int y0 = 26;

  // Coals first, under the art. They breathe on their own slower beat so the
  // eyes do not pulse in lockstep with the flames.
  uint32_t gh  = (millis() / 190) * 2654435761u;
  int      lvl = (int)((gh >> 16) % 3);
  for (int i = 0; i < 3; i++) {
    const DeathSocket& s = DEATH_SOCKET[i];
    if (!s.coal) continue;
    int gx1 = x0 + s.c0 * DEATH_ADV,   gx2 = x0 + s.c1 * DEATH_ADV;
    int gy1 = y0 + s.r0 * DEATH_PITCH, gy2 = y0 + s.r1 * DEATH_PITCH;
    canvasRect(gx1 + 6, gy1,     gx2 - 6, gy1 + 5, DEATH_HOLLOW, true);
    canvasRect(gx1,     gy1 + 5, gx2,     gy2 - 5, DEATH_HOLLOW, true);
    canvasRect(gx1 + 6, gy2 - 5, gx2 - 6, gy2,     DEATH_HOLLOW, true);
    // The coal itself: a squat lozenge rather than a block, because a bright
    // rectangle inside the socket reads as a sticker pasted on the skull. The
    // hot part is a slit, which is what a coal looks like edge-on.
    int cx = (gx1 + gx2) / 2, cy = (gy1 + gy2) / 2;
    canvasRect(cx - 5, cy - 4, cx + 5, cy - 2, DEATH_COAL_MID[lvl], true);
    canvasRect(cx - 8, cy - 2, cx + 8, cy + 2, DEATH_COAL_MID[lvl], true);
    canvasRect(cx - 5, cy + 2, cx + 5, cy + 4, DEATH_COAL_MID[lvl], true);
    canvasRect(cx - 4, cy - 1, cx + 4, cy + 2, DEATH_COAL_HOT[lvl], true);
  }

  for (int r = 0; r < DEATH_ROWS; r++) {
    const char*      line = DEATH_ART[r];
    const DeathBand& band = DEATH_BAND[r];
    const int zone = (r < DEATH_ZONE_FACE) ? 0 : (r < DEATH_ZONE_JAW ? 1 : 2);
    // Per-row firelight beat, for the warm rim along the edge of the bone.
    uint32_t rh  = (uint32_t)r * 2246822519u ^ (phase * 374761393u);
    bool     lit = ((rh >> 19) & 3) != 0;

    for (int c = 0; c < DEATH_COLS; c++) {
      char ch = line[c];
      if (ch == '\0') break;
      if (ch == ' ')   continue;

      uint32_t col;
      if (band.lo <= band.hi && c >= band.lo && c <= band.hi) {
        int edge = (c - band.lo < band.hi - c) ? (c - band.lo) : (band.hi - c);
        if (edge < DEATH_RIM)             col = lit ? DEATH_RIM_HI[zone] : DEATH_RIM_LO[zone];
        else if (deathSocketLit(r, c))    col = DEATH_RIM_HI[zone];
        else                              col = DEATH_BONE[zone];
      } else {
        // Per-glyph shimmer, plus a gust that sways neighbouring columns
        // together on a slower beat so the plume moves as well as sparkles.
        uint32_t h = (uint32_t)r * 73856093u ^ (uint32_t)c * 19349663u
                   ^ (phase * 83492791u);
        h ^= h >> 13;
        uint32_t g = (uint32_t)c * 2654435761u ^ ((phase / 3) * 40503u);
        int tier = deathHeat(r, c) + (int)(h % 3) - 1 + (int)((g >> 11) % 3) - 1;
        // Tier 0 belongs to the embers alone -- plume allowed up there too
        // would put near-cream strokes against the bone it is licking, and
        // the two would read as one shape.
        if (deathIsEmber(ch)) { if (tier < 3) tier = 3; tier -= 3; }
        else if (tier < 1)    tier = 1;
        if (tier > DEATH_FIRE_N - 1) tier = DEATH_FIRE_N - 1;
        col = DEATH_FIRE[tier];
      }
      canvasChar(ch, x0 + c * DEATH_ADV, y0 + r * DEATH_PITCH, col);
    }
  }

  // Who it was, and the line the chronicle set down for them.
  int ny = y0 + DEATH_ROWS * DEATH_PITCH + 12;
  char nm[16];
  strlcpy(nm, DeathUI::_who, sizeof(nm));
  for (char* p = nm; *p; p++) *p = (char)toupper((unsigned char)*p);
  canvasText16(nm, (240 - (int)strlen(nm) * 12) / 2, ny, 0xF2B84C);
  if (DeathUI::_line[0]) {
    char ln[48];
    strlcpy(ln, DeathUI::_line, sizeof(ln));
    canvasText8(ln, (240 - (int)strlen(ln) * 6) / 2, ny + 24, 0xBC7A38);
  }
  static const char TAG[] = "the dark keeps it";
  canvasText8(TAG, (240 - (int)(sizeof(TAG) - 1) * 6) / 2, ny + 40, 0x80421A);
}
