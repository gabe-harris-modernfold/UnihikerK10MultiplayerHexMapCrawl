# The Meridian Engine — spec

The hidden cause behind the wasteland, and the way out of it. Covers the
Engine's seven nodes, the signature sites that hide them and the quests that
activate them, how sites are revealed (one per 1,000 points), the Meridian
Pulse, per-player belief, and the end of the game: with all seven nodes
active, a survivor with **10,000 points** reactivates the Engine and escapes
alone through the gateway into prewar time.

Status: **design draft, nothing implemented.** No firmware, worldgen, client,
content, mock-server or bot changes exist yet. Open questions are at the
bottom.

**The Engine is its own system.** It does not touch Creeping Doom: no shared
state, no shared voice, no shared events, and neither reacts to the other.
It also keeps its own clock, separate from the Threat Clock.

---

## The lore

The public history says nuclear war broke civilization. The deeper truth is
stranger.

Before the war, an international research complex tried to build a
planetary field resonator out of synchronized particle accelerators, buried
antenna arrays, orbital clocks and geometric megastructures. It was meant to
stabilize communications and distribute energy. During the exchange, its
autonomous control system activated every surviving node at once. It
preserved one region by sealing it into a topological loop, and it trapped
radiation, weather systems, electromagnetic echoes and fragments of human
cognition inside that loop.

That region is the map. Reactivating the Engine is the only way out.

### It is never settled what the Engine is

The game never confirms one reading. Five stay in play:

| # | Reading | In one line |
|---|---|---|
| 1 | **Machine** | A damaged physical machine that survivors describe in religious terms. |
| 2 | **Ritual** | A ritual object people mistakenly describe as technology. |
| 3 | **Resonance** | A machine that found ritual geometry actually works. |
| 4 | **Prison** | A cage built around something older than humanity. |
| 5 | **Mirror** | A narrative trap that alters the evidence to match what the observer believes. |

Readings 1–4 are **lenses** a player can come to believe. Reading 5 is not a
lens. It is the rule the whole system runs on (see
[Belief](#belief-per-player)). The machine's geometry echoes labyrinths,
mandalas, ritual circuits and protective seals: a journey to a center and
back, and coming out changed. That is the shape of a session.

---

## The mechanic in one paragraph

Seven **nodes** sit on a hidden geometric figure: six on a hexagon and the
**Meridian** at its center. Each outer node lies under a **signature site**,
a place with its own people and its own quarrel over what the Engine is: a
temple of dials, a buried repository, a motel caught in time, an orchard, a
farmhouse full of footprints. Each node is dormant until its site's
**quest** is completed, and the quests are hard: several stages, gated by
weather, pulses, items, factions and sometimes by more than one survivor
acting together. Sites stay hidden until a survivor has earned them: **every
1,000 points reveals one more site to that survivor.** The six outer quests can be done in any
order; the Meridian's opens only once they are finished. Every so often all
awakened nodes fire at once, a **Meridian Pulse** that every phone and the
K10 feel at the same instant. Quest choices pull each player's private
**belief** toward one of four lenses, so each survivor reads a different
story about what they are rebuilding. Once all seven nodes are active, the
first survivor at **10,000 points** reactivates the Engine, the gateway
opens, and they escape into prewar time. The game ends for everyone, and
each player reads the ending through their own lens.

## How the story maps onto the game

| Lore element | In the game | New or existing |
|---|---|---|
| Geometric megastructures | Seven nodes on a hidden figure (worldgen) | **New** |
| Machine components and trapped forces | The signature sites, one per node | **New** |
| Energy distribution | The Cathedral of Dials | **New** site |
| Particle accelerators | The bunker tunnel board, retold as the beamline (sixth-node placeholder) | Existing, text plus one quest |
| Buried antenna arrays | Hatches and vent shafts as antenna heads | Existing hexes |
| Orbital clocks | The Meridian Clock that schedules pulses; the Selfsame Motel, where the loop's hours bunch up | **New**, not the Threat Clock |
| All nodes fired at once | The Meridian Pulse | **New** |
| The topological loop | The map's wrap-around edges (`hexDistWrap`, `wrapQ`/`wrapR`) | Existing, made part of the story |
| Trapped radiation | The Place Without Honor; radiation weighted along the figure and the wrap seam | **New** site, worldgen weighting |
| Trapped contaminants | Orchard Nine | **New** site |
| Trapped weather | Lightning as a power route at the Cathedral; later, recurring named storms | Existing weather phases |
| Electromagnetic echoes | Radio fragments | **New** content |
| Fragments of cognition | The House of Returning Footsteps; the earlier expedition in the Motel | **New** sites |
| Stabilize comms, distribute energy | Active nodes recharge fuel and extend vision | **New**, small |

---

## The seven nodes

### Placement (worldgen)

- **7 nodes**: six on a hexagon, plus the **Meridian** at its center.
- The map is a 75×57 torus, so "the center" is arbitrary. The figure is what
  defines one. Pick a random anchor, then place the six outer nodes at a
  fixed hex radius (about 18–22) at 60° steps, snapping each to the nearest
  passable hex.
- Each outer node is assigned one of the six outer sites at random, so the
  figure and the site layout change from map to map.
- Each site stamps a small terrain patch around its node so it reads as the
  place it is (see the site table below). The figure should be
  discoverable, not hidden by terrain noise.
- Keep nodes at least `HATCH_MIN_DIST` from hatches and from each other.
- The figure is **never drawn for the player**. Sites appear one at a time
  as the survivor's score climbs (see [Revealing sites](#revealing-sites)).
  Someone who has three or four on their map should start to see the
  hexagon on their own. That realization is the real reveal.

Storage: a fixed `meridianNodes[7]` array, like `bunkerHatches[]`, **not** a
`HexCell` field. `HexCell` is wire-encoded and changing it bumps `MAP_BYTES`
and the save.

### Revealing sites

Sites are hidden until earned. **Every 1,000 points a survivor earns reveals
one more site to that survivor**:

| Score | Revealed to that survivor |
|---|---|
| 1,000 – 6,000 | One outer site per 1,000, six in all |
| 7,000 | The Meridian |
| 10,000 | (the gateway; see [Score and the gateway](#score-and-the-gateway)) |

- **Reveals are per survivor.** Each survivor has their own reveal count,
  driven by their own score. Survivors at different scores have different
  sites on their map.
- **An unrevealed site does not exist for you.** Walking onto its hex shows
  plain terrain and no encounter. Only survivors who have a site revealed
  can see it, enter it or advance its quest.
- **Which site comes next:** the nearest unrevealed outer site to where the
  survivor is standing when they cross the threshold. The Meridian is always
  seventh. Because the six outer sites sit on a hexagon, a survivor's reveals
  trace the figure outward from wherever they happen to be.
- **Progress is still shared.** Once a site is revealed to you, you see its
  real state (Awake, Active, the stage it's at), including work other
  survivors did before you could see it.
- The terrain patch each site stamps is visible to everyone from the start.
  An odd ring of glass or an orchard in the wrong place is a hint, not a
  site.
- On reveal, the survivor's phone shows the site's name and a line in their
  lens, and the site appears on their map.

Quest completion pays 1,000 (see the score table), so finishing one site is
usually enough to reveal the next. A survivor who follows the Engine gets
pulled from site to site. One who ignores it sees the figure much later.

### Node state (global, shared by the party)

| State | Meaning |
|---|---|
| **Dormant** | No survivor who can see it has started its quest. |
| **Awake** | Its quest has started. Shown to every survivor it is revealed to. |
| **Active** | Its quest is complete. It hums, shows on the map of every survivor it is revealed to, and takes part in every pulse. |

Activation is **shared**. Any survivor can advance any quest, and progress
belongs to the party. That keeps a cooperative game cooperative: the race to
10,000 is individual, but the Engine is rebuilt together.

### The machine still half-works

- **Energy:** standing on an Active node slowly recharges fuel.
- **Comms:** vision +1 while standing on an Active node.

Both are small. They make nodes worth passing through, not worth camping on.

---

## The quests

Each quest is a chain of **stages**. A stage is one of:

- a Meridian encounter at the site (same engine and press-your-luck shape as
  biome encounters, files in `data/encounters/meridian/<site>/`)
- a delivery (carry a quest item to a hex)
- a condition (be on a hex while some world state holds)

A quest's current stage is shown to anyone standing on its node, in their
lens.

### What makes them hard

- **3–4 stages each**, and some happen away from the site.
- **High DNs.** Key checks sit at `base_risk` 65–80 (about DN 9–10).
- **They cost things.** Resources, fuel, medicine and quest items are spent,
  not checked.
- **World-state gates.** Some stages only work during a storm, at dusk, or
  on a pulse, so a player has to plan and wait.
- **Consequences that stick.** Most sites have people in them, and how a
  player gets what they need changes the site for everyone afterwards.
- **Setbacks, not deaths.** A failed check costs progress (a stage drops
  back one, a quest item is destroyed) far more often than it costs LL. The
  tension data already shows too many deaths and too few near-misses
  (bot-testing.md, 1:1.03 against a target nearer 1:4). Quests should create
  the near-misses, not add to the deaths.

### One survivor can do everything

Activation is shared, and **every quest, including the Meridian, can be
completed by a single survivor**. No stage has a minimum headcount. Extra
survivors help (they split the work, carry more, take turns absorbing
radiation) but are never required. The same survivor may complete all seven.
This also means a two-player or solo game can always finish.

---

## The signature sites

Each outer node lies under a site. A site is a single hex on the map; what
happens inside it is encounter chains plus a small amount of site state.
Five sites are written. The sixth outer slot and the Meridian are still open
(see [Open slots](#open-slots)).

| Site | What of the Engine it hides | Leans toward | Terrain patch | Hard because |
|---|---|---|---|---|
| **The Cathedral of Dials** | Energy distribution | Machine vs Ritual (Resonance) | Broken Urban | A priesthood in the way; every route to power costs something |
| **The Place Without Honor** | Its buried waste, or its prisoner | Prison vs Machine | Glass Fields and NukeCrater ring | A four-layer descent where radiation rises with every layer |
| **The Selfsame Motel** | The loop's timekeeping | Mirror | Open Scrub by an old road | A sequence puzzle rebuilt from scattered clues |
| **Orchard Nine** | Field filtration | Ritual vs Machine | Rust Forest beside a River Channel | A succession crisis before the settlement will give up the graft stock |
| **The House of Returning Footsteps** | Its record of the people it trapped | Mirror and Prison | Open Scrub or Ridge farmland | Reading the right trail, with a shortcut that costs a wound |

Each site's choices are still tagged across all four lenses. "Leans toward"
is the quarrel the site is *about*, not a restriction.

### The Cathedral of Dials

A power-distribution control room has become a temple. Every analog gauge is
an eye, every breaker a vow, and acolytes keep the voltage stable without
understanding the grid. The grid still works.

**Engine role.** The Cathedral switches load for this segment of the Engine.
The node wakes only when its power is switched back onto the Engine bus.

**Quest.**

1. **Enter.** Leave an offering (scrap and fuel) or slip in unseen
   (Navigate).
2. **Get the power.** One of four routes:

   | Route | How | Cost and consequence | Lens |
   |---|---|---|---|
   | **Steal batteries** | One Scavenge check at high DN | Fast. The Cathedral turns **hostile**: it closes for good and stage 3 runs at +1 DN. | none |
   | **Negotiate** | Deliver fuel, scrap and medicine over two visits | Expensive and slow. The Cathedral stays friendly. | Ritual |
   | **Expose the priesthood's errors** | Find the failing relay the liturgy has grown up around (Scavenge, then Endure to face the crowd) | Power flows, but the priesthood **splits** and half the acolytes leave. | Machine |
   | **Learn the liturgy** | Attend the switching liturgy at dusk on three separate game-days | Safe but slowest. Reveals the liturgy is a compressed maintenance protocol. That survivor gets −1 DN on machine checks at every other site. | Resonance |

3. **Switch the load.** Run the switching sequence at the node. During a
   storm, a survivor can catch lightning instead of using batteries. That's
   more dangerous and cheaper, and it is where the trapped weather comes in.

**Afterwards.** A friendly Cathedral sells fuel recharges for scrap. A split
one does the same at a worse rate. A hostile one is closed.

**Site state:** disposition (unknown / friendly / split / hostile), liturgy
visits per player.

### The Place Without Honor

A square wasteland of leaning stone teeth around a deep repository. Four
layers of warning survive. The outer culture says the stones imprison a god.
The inner priesthood says the god is a metaphor. The Geiger counter says both
should stop digging.

**Engine role.** Where the Engine's waste was buried, or, in the Prison
reading, what the Engine was built to hold. The node is at the bottom.

**Quest.** The purest press-your-luck scene in the game: a descent through
the four warning layers. Radiation goes up with every layer (+1, +2, +3,
+4).

| Layer | The warning | Check | Lens |
|---|---|---|---|
| 1 | **Emotional architecture**: spikes, dread, ground that feels wrong | Endure | Ritual |
| 2 | **Pictographs**: faces in pain, a figure digging | Navigate, to find the true shaft. Misreading costs radiation. | Machine |
| 3 | **Multilingual fragments**: the same warning in languages nobody speaks | Scavenge, or spend a radio fragment to translate | Prison |
| 4 | **The technical archive**: the activation manual | The final check, DN 10 | Machine |

- **Progress is saved by layer and shared by everyone.** A survivor can
  bank out at any layer, and whoever goes down next starts there. Descending
  in relays (one goes down, comes back up to heal, another continues) is the
  intended way through, and a solo survivor can relay with themselves.
- **The outer culture**, at the surface, lets you pass for an offering. If
  you refuse, they curse you, and your first layer costs double radiation.
- **The inner priesthood**, at layer 3, will guide you if you agree the god
  is a metaphor. That lowers layer 4's DN and tags Machine. Refusing tags
  Prison.

**Afterwards.** Activating the node reseals the repository, and radiation
around the site drops. Every lens reads that differently: the waste is
contained, the god is sealed back in, or the metaphor held.

**Site state:** deepest layer reached, whether the outer culture was paid.

This site is almost all content: encounter files plus one layer counter. It
is the right first quest to build.

### The Selfsame Motel

Every room opens into the same room at a different hour.

**Engine role.** The Engine's orbital clocks fell out of sync, and the Motel
is where the loop's hours bunch together.

**Quest.** A planning puzzle.

- The room has a hidden **safe sequence**, generated per map: 4 hours, in
  order. Each hour needs one correct **action** and one **object left
  behind** for the next hour.
- An **entry** is: pick an hour, take one action, leave one inventory item
  in the room. Each survivor gets **one entry per hour**, so a solo survivor
  can walk the whole sequence by themselves, meeting the objects their own
  earlier self left. With more survivors the hours can be split up, which
  is faster but needs coordination.
- **Clues are per player.** Examining the Motel exterior (a free
  encounter) shows each survivor a different clue on their own phone: a
  clock stopped at a particular hour, a bloodstain by the bed, a key on the
  wrong hook. A solo survivor gathers them all over several visits. A group
  can pool them by talking, which is quicker but only works if people
  compare notes out loud.
- **Supplies for future selves.** Any item left in an hour room can be
  picked up by whoever enters a later hour, including the same survivor.
- **Rescue an earlier expedition.** One hour room holds an earlier
  expedition. If this save has dead survivors, it is **them**, by name,
  caught at an hour of their own. Rescuing them pays a radio fragment and
  belief. With no dead survivors, it is a prewar survey team.
- **Paradox Scar.** A wrong action or wrong object breaks the sequence. The
  survivor who broke it takes a **Scar**, and the attempt resets: objects
  are gone and everyone's entries return. The hidden sequence **does not
  change**, so what was learned carries over.

**The Scar.** A lasting mark on one survivor: +1 DN on every Engine quest
check, stacking to 3. Riding a pulse on an Active node clears one; the Engine
resyncs you.

**Afterwards.** The room keeps its hours. Anything left there stays, which
makes it a stash that survives across the session.

**Site state:** the hidden sequence, room objects, entries used per survivor
per hour, and whether the expedition was rescued. This site has the most new
code of the five and should be built late.

### Orchard Nine

A suburban research orchard grows fruit that absorbs specific contaminants.
Eaten raw, the fruit is poisonous. Processed correctly, it filters water. A
peaceful settlement has built its identity around ritual pruning, and it
will not surrender the graft stock unless the succession crisis is solved.

**Engine role.** The orchard was planted along the Engine's field lines to
filter what the loop trapped. The dead mother tree at its heart stands on
the node; grafting new stock onto it wakes the node.

**Quest.**

1. **Arrive.** While friendly, the Orchard works as a Settlement for trade
   and crafting.
2. **The succession crisis.** The old pruner has died. There are four
   claimants, one per lens, and each has an encounter where a survivor can
   dig into their case (walk the rows at night, read the pruning journals,
   test the fruit):

   | Claimant | Their case | Lens |
   |---|---|---|
   | The Archivist | The orchard is a research station; keep the records | Machine |
   | The Apprentice | The pruning liturgy is the orchard's life; keep the cuts | Ritual |
   | The Grafter | The rows follow a pattern in the ground; extend it | Resonance |
   | The Burner | The roots are holding something down; burn it before it wakes | Prison |

3. **Back a claimant.** A survivor who has investigated at least two
   claimants may back one. With several survivors, most backers wins, and a
   tie splits the settlement and sends the stage back one. A solo survivor
   decides alone.
4. **Graft the mother tree.** A Forage check at DN 9 using the graft stock
   the new leader hands over.

**What the choice changes.** The Burner winning activates the node fastest,
but the orchard burns, and with it the fruit and the Settlement. Any other
winner keeps the Orchard open.

**The fruit.** A new item, *Orchard fruit*, found at and near the site.
Eating it raw is poison (radiation and LL). The Orchard teaches a recipe,
learned the way recipes already are, through an encounter `recipe` loot
entry (see `data/recipes.cfg`), that turns fruit into a **water filter** at
any Settlement. Thirst already kills survivors, so this gives the site value
well beyond the quest.

**Site state:** claimants investigated per survivor, backers, the winner,
and whether the orchard burned.

### The House of Returning Footsteps

A farmhouse holds every set of tracks ever made inside it.

**Engine role.** The Engine's record of the people it trapped, leaking into
one building. The node is at the top of a stairway that no longer exists.

**Quest.** Read the right trails. Four sets of footprints, each its own
encounter branch, all on Navigate at rising DN:

| Trail | Where it leads | Pays | Lens |
|---|---|---|---|
| Heavy boots, circling | **A sealed pantry** | Food and water | Machine |
| Two sets, one dragged | **A murder** | A radio fragment and the truth of what happened | Prison |
| Small bare feet | **A child's hiding place** | A radio fragment; the child is an echo, and the scene is gentle | Ritual |
| Steps that start mid-floor | **The stairway that no longer exists** | The node | Resonance |

The stairway trail opens only after **two** of the other three have been
followed. Each gives up part of where the stairs used to be.

**Your own future tracks.** At any check in the House, a survivor may follow
their own tracks from the future instead. The check succeeds automatically,
but they take a **Foretold Wound**: the next hazard they suffer within 2
game-days always inflicts a major wound, and if none comes, a minor wound
arrives at dawn anyway. It uses the existing wound tiers, and the trade is
spelled out on screen before they choose.

**Afterwards.** The pantry slowly refills. The House also starts showing the
footprints of survivors who have died on this map.

**Site state:** trails followed, and Foretold Wound timers per survivor.

### Open slots

**The sixth outer site** is unwritten. Until then it uses the **Beamline**
placeholder: descend into the tunnel board, recover a field coil (heavy,
2 inventory slots) from the deepest reachable tunnel, and seat it at the
node. Aligning three hatches as antenna heads can be one of its stages.
Note that encounters cannot load underground today: `encLoadFile()` rejects
terrain ≥ 10 (trap-system-spec.md, "Tunnels are currently unreachable").
The Beamline needs that fixed first, or should keep all its encounters on
the surface.

**The Meridian** has no site yet. Its rules:

- Opens only when all six outer nodes are Active.
- One long encounter chain whose DNs climb toward DN 10, with banking
  allowed only at a few nodes.
- It draws on the other sites. For example, the Cathedral liturgy lowers a
  DN, a Motel object can be carried in, and the Orchard's leader or the
  House's echoes appear in the final scenes.
- Whoever is present answers the final choices **in their own lens**: the
  same scene, a different ritual or procedure on every phone.
- A failure knocks the chain back a stage and fires an off-schedule pulse on
  everyone standing nearby.

Completing it makes all seven nodes Active. From then on, the gateway can
open.

The placeholder quests from the earlier draft are folded into the sites:
Storm into the Cathedral's lightning route, Clock into the Motel, Glass into
the Place Without Honor, Echo into the House and the Motel's earlier
expedition, and Array into the Beamline.

---

## The Meridian Pulse

"Activated every surviving node at once" becomes the game's heartbeat.

- **Cadence:** real-time, not game-days. Game-days can fly by when every
  player rests (see bot-testing.md, day length). Every 15–20 real minutes
  with jitter, so a 2-hour session gets about 6–8 pulses. It never fires in
  the first 10 minutes.
- **Warning:** about 60 seconds ahead, the K10 LEDs start a slow sweep and a
  tone plays, and phones show a warning. This is the part only this hardware
  can do: six people in one room all get the jolt together.
- **At the pulse:**
  - Anyone within radius 2 of an **Awake or Active** node takes a radiation
    hit.
  - Anyone standing **on an Active node** instead *rides the pulse*: a
    score bonus and a radio fragment in their lens.
  - Pulse-gated quest stages resolve.
  - Every client flickers the map and plays the pulse sound. The LCD shows
    the pulse animation.
- **More nodes, stronger pulses.** Radiation and the ride bonus scale with
  the number of Active nodes, so the world gets more dangerous as the party
  gets closer to escape.
- **Underground:** the beamline hums. Players in tunnels take no radiation
  and get a flavor line only.
- The pulse never moves, wakes or otherwise touches Creeping Doom.

State: `pulseNextMs`, `pulseCount`. Only `pulseCount` is saved; the timer
rebases on load, the same way the weather gap does.

---

## Belief (per player)

Each player carries a hidden belief vector with one value per lens:

```
uint8_t belief[4];   // Machine, Ritual, Resonance, Prison; saturating
```

### How it moves

- **Tagged choices.** Any encounter choice may carry `"lens": "ritual"`.
  Picking it adds to that lens. Quest encounters tag nearly every choice;
  biome encounters can tag a few where it fits naturally. For example, at a
  node: *measure the field* (Machine), *walk the circuit barefoot*
  (Ritual), *align the arrays to the pattern in the glass* (Resonance),
  *reinforce the seal* (Prison).
- **Confirmation.** Riding a pulse adds a little to the current dominant
  lens. The Engine shows you what you already believe, which makes belief
  self-reinforcing on purpose.

### What it does

The player's **reading** is their dominant lens, if it leads the runner-up by
a margin (for example 3). **Otherwise their reading is Mirror.** An undecided
player sees the trap itself: text that contradicts itself and rearranges as
they read it.

Lore text can then vary by reading:

```json
"text": "A ring of basalt pylons, humming.",
"text_by_lens": {
  "machine":   "Accelerator housing, segment 4 of 12. The coolant lines are still cold.",
  "ritual":    "Seven stones and a worn path between them. Someone walked this circuit for years.",
  "resonance": "The pylons stand exactly where the glass fractures meet. That is not a coincidence.",
  "prison":    "The pylons all lean inward. Whatever they hold, they are still holding it.",
  "mirror":    "The inscription reads differently each time you look away."
}
```

`text` is the fallback for any lens without a variant. Choice labels can
vary the same way (`label_by_lens`): the mechanics of a choice stay
identical, only its description changes.

### The rule that makes it work

**Two players on the same quest read different evidence.** They're in the
same room on their own phones. When they compare notes out loud they
disagree about what they are rebuilding, and the game never says who is
right. This is the Mirror reading turned into a mechanic, and it is why
belief has to be per player. It lands hardest where people act together:
backing a claimant at Orchard Nine, splitting the Motel's hours, or standing
in the Meridian's final scene, each of them reading it differently.

### Keeping it private

Today the client fetches whole encounter files from `/enc` (the
`game-server.hpp` handler reads them straight off the SD card). Sending
`text_by_lens` that way would put every variant on every phone. Lens text
must be resolved **on the server** before it reaches a client. See Open
questions for the two ways to do that.

Belief is **never shown as numbers**. Players see their reading only through
the text they get, plus a fragment journal written in their lens.

Mechanical effects of belief are **out of scope for v1**. Lenses change what
you read, not your odds.

---

## Radio fragments

Short pieces of prewar transmissions, clipped and on repeat: a shift log, a
calibration countdown, a broadcast cut off mid-sentence. Sources:

- riding a pulse
- quest encounters, especially the deepest node of each
- searching an Awake or Active node, or a hatch (the antenna arrays)
- the first time each player crosses the map's wrap seam: *"The ridge ahead
  is the one you left three days ago."*

Fragments can also be spent, for example to translate layer 3 of the Place
Without Honor. Each fragment has lens
variants like any other lore text. Collected fragments go in a per-player
journal, which is the closest thing the game has to a lore log. A fragment
id is a byte and a player holds at most ~32 of them, which fits in a
bitmask.

---

## Score and the gateway

### Thresholds

| Score | What happens |
|---|---|
| every 1,000 up to 6,000 | an outer site is revealed to that survivor |
| 7,000 | the Meridian is revealed to that survivor |
| 10,000 | that survivor can reactivate the Engine (both locks below) |

**The first reveal is the pacing problem.** Under today's economy, bots take
about a whole run to reach 1,000 (bot-testing.md, Findings), so the first site
would appear very late. Either the base game has to pay faster early on, or
the first threshold should come sooner (for example 500) with the rest at
1,000 intervals. Bots should measure **time to first reveal**.

### Two locks on the gateway

The gateway opens for a survivor only when **both** hold:

1. **All seven nodes are Active.** The party's shared work.
2. **That survivor has 10,000 points.** Their own charge. The Engine reads
   resonance, and every point a survivor earns is resonance.

Score keeps counting before the nodes are done, so a survivor can reach
10,000 early and wait on the party, or the nodes can finish while everyone
is still short.

### Score sources

Existing score sources keep working; the Engine adds the big ones:

| Source | Points (placeholder, tune with bots) |
|---|---|
| Wake a node (first survivor to start its quest) | 150 |
| Complete a quest stage (everyone who took part) | 300 |
| Complete an outer quest (everyone who took part in the final stage) | 1,000 |
| Complete the Meridian (everyone present) | 2,000 |
| Ride a pulse on an Active node | 200 |
| Recover a radio fragment | 50 |
| First wrap-seam crossing | 100 |

**This is a real rebalance, not a tweak.** Measured score today is about
5 × steps, and bot runs are set up around a 1,000-point target (see
bot-testing.md, Findings). 10,000 from existing sources alone is
~2,000 steps, far more than a 2-hour session. Quests have to supply most of
the gap. That also fixes an existing finding: *"Engaging with content costs
about half your score."* Once quests are the fast road to 10,000, engaging
with content becomes the way to win.

Downed survivors already keep their lifetime score (`handleMsg_pick`,
network-msg-player.hpp). Dying costs time, not the charge.

### Reactivation (the end of the game)

The moment both locks hold, reactivation fires. That is the first survivor
to reach 10,000 after all seven nodes are Active, or, if the seventh node
activates while several survivors are already past 10,000, whoever has the
highest score.

1. Every node fires at once, one last time: the full pulse effect on every
   client, plus a unique LED and tone motif on the K10.
2. The gateway opens and that survivor steps through into prewar time.
3. **The game ends for everyone.** All actions are refused from then on.
4. **Endings are per player, by reading.** The survivor who escaped reads
   what the gateway was according to their lens; everyone left behind reads
   what they saw close. Examples:
   - *Machine:* the loop's clock resyncs, and the escapee wakes on the
     morning of the test.
   - *Ritual:* they reached the center of the labyrinth and walked out the
     other side.
   - *Resonance:* the geometry held, and the pattern carried them home.
   - *Prison:* the seal opened to let them out, and something else came
     out with them.
   - *Mirror:* they arrive in a prewar world exactly as they imagined it,
     which should worry them.
5. **The LCD shows the escapee's ending**, the public version. Phones show
   each player's own. The shared screen tells one story; every private
   screen tells another.

A new map, and a new figure, start only through the existing reset path.

### Implementation notes for the end check

- The check has to run wherever score changes **and** when the seventh node
  activates. `addScore()` (survival_skills.hpp) is the intended choke point,
  but `collectResource` (survival_state.hpp) writes `p.score += gain * 10`
  directly. Route that through the same check, or run the check once per
  game tick instead.
- **Only the winner escapes.** Everyone else is left behind, whatever their
  score and whether or not they helped activate the nodes.
- Ties on the same tick go to the first player processed. The game loop is
  single-threaded, so this is deterministic.
- `score` is a `uint16_t`, so 10,000 fits.
- The game-over state must be **saved** (`SAVE_VERSION` bump, currently 18),
  so a reboot shows the finished game instead of resuming it.

---

## Data layout

| State | Where | Saved |
|---|---|---|
| `meridianNodes[7]` {q, r, siteId, state, stage} | new `MeridianEngine` struct in its own `meridian-engine.hpp`, **not** in `WorldSystem W` | yes |
| Per-site state: Cathedral disposition, Place Without Honor layer and toll, Motel sequence + room objects + entries used, Orchard claimants + backers + winner, House trails followed | same struct, one small block per site | yes |
| `pulseCount`, `pulseNextMs` | same struct | count only |
| `gameOver`, `escapeePid` | same struct | yes |
| `belief[4]`, `fragmentMask` (32 bit), `seamCrossed` | `Player` | yes |
| `revealedMask` (7 bits, one per node) | `Player` | yes |
| `scars` (0–3), `foretoldWoundDays`, `liturgyVisits`, `liturgyKnown` | `Player` | yes |
| Quest items (batteries, graft stock, Orchard fruit, water filter, field coil) | `items.cfg`, as ordinary inventory items flagged not tradeable to the caravan | with inventory |

About 12 bytes per player plus ~100 global (the Motel is the largest block). Worldgen, save and load each gain
one block. Everything bumps `SAVE_VERSION`.

## Wire protocol

New events. Names are illustrative; ids go after `EVT_DAMAGE` (31):

| Event | Payload | Who |
|---|---|---|
| `mer_reveal` | node index, q/r, site id, **resolved** reveal line | self, on crossing a 1,000 threshold |
| `mer_node` | node index, state, stage | survivors the node is revealed to |
| `mer_quest` | node index, stage, **resolved** stage hint | all (progress), self (hint text) |
| `mer_warn` | seconds to pulse | all |
| `mer_pulse` | pulse count, per-player outcome (rad / ride / none) | all |
| `mer_frag` | fragment id + **resolved** text | self |
| `game_over` | escapee pid, **resolved** personal ending | each player gets their own |

New refusal codes: action after game over; quest stage not ready (wrong
weather, not a pulse, wrong time of day); Motel hour already entered; site
closed (hostile Cathedral, burned Orchard). An unrevealed site does **not**
refuse; it simply isn't there, and the hex behaves as plain terrain.

**Bots:** the arena currently ends runs on its own `--target`. Once the
firmware ends the game, `arena.py` has to recognize `game_over`, oracles
need rules for the new events and refusals, and a policy has to learn to
run quests: pathing to nodes, carrying items, waiting on weather and
pulses, and choosing routes at each site. Because one survivor can do
every quest, a single policy can be tested solo before running a fleet.
Bots must only path to sites in their own `revealedMask`. The new headline metrics are
**time to first reveal**, **time to all seven nodes** and **time to
escape**, alongside the tension
arc, targeting about 2 hours to escape. Read bot-testing.md first; several
refusals there are silent, and the "stage not ready" refusal must not be
one of them.

## Content library

- `data/encounters/meridian/<site>/`: the encounter files for each site's
  stages, with choices tagged with lenses throughout. Meridian encounters are
  not keyed by terrain like biome pools, so they need their own load path
  next to the `index.json` one.
- The Meridian file: the longest scene in the game.
- ~24 radio fragments to start, each with five variants (four lenses plus
  Mirror).
- Seven quest hint lines per stage, per lens, plus a reveal line per site
  per lens.
- Motel clue pool (enough for 4 hours of sequence per map, several clues per
  hour), and names for the Orchard claimants and the Cathedral's acolytes.
- Five endings, each in two forms: *escaped* and *left behind*.
- A retelling pass on tunnel and hatch text so the beamline and antenna
  story holds together.

**Tone:** clinical and sacred at once. Machine text reads like a maintenance
log; Ritual text like a pilgrim's notes; Resonance like a physicist's
margin scrawl; Prison like a warning sign; Mirror contradicts itself. No
lens is ever written as obviously wrong.

## Phasing

1. **Nodes and one site:** worldgen figure, node state, per-survivor
   reveals at 1,000-point thresholds, the Place Without Honor end to end
   (encounters and one layer counter, no world-state gates), the two-lock
   gateway and game over. Playable, with the node requirement temporarily
   set to 1.
2. **The Pulse:** clock, warning, effects, K10 LEDs and tone. Scars clear
   on a pulse, so the Motel depends on it.
3. **The remaining sites,** roughly by new code needed: House of Returning
   Footsteps, Cathedral of Dials, Orchard Nine, the sixth site, the Selfsame
   Motel, then the Meridian.
4. **Belief:** lens tags, server-side lens resolution, Mirror fallback,
   per-player endings.
5. **Echoes:** radio fragment journal, seam event, then recurring storms and
   echoes of dead survivors.

Bot support lands with each phase, not at the end.

---

## Decided

- **Node activation is shared** by the party.
- **One survivor can complete every quest**, including all seven; no stage
  has a minimum headcount.
- **Only the winner escapes.**
- **Sites are revealed per survivor, one per 1,000 points.**

## Open questions

1. **The sixth site and the Meridian's site.** Five sites are written. The
   Beamline stands in for the sixth, and the Meridian has rules but no
   place yet.
2. **Reveal order.** As written, each reveal is the nearest unrevealed outer
   site. Alternatives: a fixed order around the hexagon (a labyrinth walk),
   or random.
3. **Can survivors help at sites they can't see yet?** As written, no: an
   unrevealed site does not exist for you, so a low scorer can't join a high
   scorer's quest. Letting a survivor enter a site while standing on it with
   someone who has it revealed would make co-op easier and weaken the
   reveal.
4. **Instant end, or walk to the gateway?** As written, reactivation fires
   the moment both locks hold. The alternative: the gateway opens on the
   Meridian and the winner must walk there, which gives the others a last
   window and a final chase.
5. **How hard is hard?** Seven multi-stage quests plus 10,000 points inside
   2 hours is tight, especially when one survivor may be doing most of it.
   Bots will show whether stage counts or DNs need to come down. The numbers
   above are starting points.
6. **The Paradox Scar.** As written it is +1 DN on Engine checks, stacking to
   3, cleared by riding a pulse. It could instead be something visible to
   other players, or tie into the Motel only.
7. **Should hostile outcomes block the quest?** Stealing from the Cathedral
   or letting the Burner win closes the site but still activates the node.
   An alternative is for a hostile site to add a stage rather than close.
8. **How is lens text resolved on the server?** (a) `/enc` takes a `pid`
   and strips the other variants before serving; simple and spoofable, which
   is probably fine for a local party game. (b) Lore text goes over the
   WebSocket instead, and the file on disk never reaches the client. (b) is
   airtight but needs more protocol work.
9. **Belief margin.** The margin for "no clear reading, so Mirror" decides
   how many players see Mirror text. It needs playtesting.
