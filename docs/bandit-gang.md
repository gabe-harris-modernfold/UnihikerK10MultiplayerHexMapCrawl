# BANDIT GANG — three roaming bandits who box you in and rob you

Status (2026-11-04): **mechanical analysis only, no code, no art.** Specced in
chat over four requests from the user:

1. Three roaming bandits, each on its own hex, who work together to slowly
   enclose a survivor. If they trap you, you lose everything.
2. The gang also sets trap encounters on the hexes it visits.
3. Its movement should feel organic, and be oriented around settlements.
4. Survivors can raid the gang's hideout, and the raid should encourage other
   players to join in.

Every choice marked *(rec)* below is my recommendation. The user has **not**
confirmed it yet; the full list is in [Open decisions](#open-decisions).

Numbers this spec leans on (firmware, 2026-11-04): the map is a 75×57 torus;
a day is `DAY_TICKS` 3000 × 100 ms = 5 min = 20 world ticks of 15 s; survivor
AP is LL + 3 (10 fresh, 5 at LL 2); terrain costs 1–4 AP, about 2 on average;
base vision radius is 1 (Ridge and Mountain +2); there are at least
`MIN_SETTLE` 27 Settlements, about one per 160 hexes, 12–14 apart.

No fighting back. Like Kane, every answer to the gang is escape, sanctuary,
terrain, timing or friends — never a fight.

---

## 1. Geometry: what "trapped" means on a hex grid

A hex has six neighbours and three bandits can't stand on all six, so each
bandit projects a **zone of control (ZOC)**: its own hex plus the six around
it. With ZOC radius 1, **three is exactly the smallest gang that can close a
ring.** Two layouts do it:

| Noose | Bandits (around the survivor P) | Result |
|---|---|---|
| **Wide** | Ring 2, three alternate *edge* hexes, 120° apart | Each covers 2 of P's 6 neighbours: all 6 covered |
| **Tight** | Ring 1, every other neighbour | 3 neighbours occupied, the other 3 each in two ZOCs |

- **Impassable hexes are walls.** River, Nuke Crater and Collapsed Tunnel
  (MC 255) count as sealed, so two bandits can close a ring against a
  riverbank. The gang should herd targets into river bends and crater rims;
  that falls out of the rule rather than being scripted.
- **Gaps.** With the bandits on ring k, the free hexes on that ring number
  6k − 9, in three gaps of 2k − 3. At ring 3 or wider a free path out
  exists. At ring 2 **every exit crosses a ZOC**, so the step from ring 3 to
  ring 2 is the dangerous moment.

## 2. The trap condition is AP, not walls

If ZOC hexes can't be entered at all, a closed ring is a guaranteed robbery.
That is binary and reads as unfair. Instead *(rec)*:

- Stepping into a ZOC hex costs **+1 AP**, and the bandit next to it
  **snatches 1 random token** as you pass.
- A bandit's own hex can't be entered.
- **The robbery ("snap")** happens when the ring is at 2 or tighter **and**
  the survivor's cheapest way out costs more than the AP they have left.

Getting out of a wide noose is two steps, one through a ZOC: about
2 × 2 + 1 = **~5 AP**. So at dawn (10 AP) you walk out, lose a token, and the
gang re-forms; at dusk (≤ 4 AP left) or wounded (LL 2 is 5 AP for the whole
day) you're caught. Even when the gang fails, being herded costs you a day.

The snap is telegraphed for one world tick (15 s) before it lands.

## 3. Speed: "slowly" is measured in AP

Survivor speed is bounded by AP per day, not by the clock: ten moves can
happen in ten seconds. A ghost's pace (1 hex per 2 ticks, 10 free hexes a
day) would outrun a survivor.

- Bandits **pay terrain costs** from a daily budget that trickles in each
  world tick. Budgets by role are in §9; they average **6 AP/day**.
- A survivor who notices and spends a day running (10 AP vs 6) **always gets
  away.** The gang catches survivors who stay put to work — foraging,
  scavenging, crafting, encounters, resting. It taxes productivity, not
  travel.
- **It has to be handicapped.** Three pursuers at equal speed can always
  catch an evader on a graph drawn on a torus, so the survivor needs
  structural outs: more AP than the gang, sanctuaries, tunnels and rivers,
  and a limit on how long the gang commits to one target.

## 4. Gang AI states

1. **Roam** the gang's territory (§9). Bandits never share a hex.
2. **Mark.** A bandit senses a target within about 5 hexes (it can reuse the
   Doom's track-heat scent), or a tripwire fires (§7).
   Target score = haul value × isolation (no other survivor within 3) ×
   exposure (not in a Settlement, not underground) × low AP.
   **Below a value threshold nobody is targeted**, so fresh respawns and
   survivors travelling light are safe, and hoarding is where the risk is.
3. **Stalk.** Move into a ring about 6 out, mostly outside the target's
   vision. Pick the best of the 6 bandit-to-slot assignments, centred on
   where the target is *heading*, with a cost term that puts one bandit
   between the target and its nearest Settlement (the cut-off, §10).
4. **Tighten in lockstep.** The ring shrinks by one only when **all three**
   are in position. That is the visible "working together", and it caps the
   gang's speed: if one bandit moved in alone, the survivor would walk out
   through the far gap.
5. **Snap** (§2).
6. **Withdraw.** Carry the loot to the hideout, slowed by the weight.
   Cooldown before the same victim is targeted again: about 3 dawns.

**Give up** after 2 days without progress, or after 3 escapes from ring 2.
An escape could pay a small score bonus so it counts as a win.

**Hold still during encounters.** While the target is in an encounter scene
the gang doesn't move — except during a raid on its own hideout (§12).

## 5. What "lose everything" means

| Option | Effect | Risk |
|---|---|---|
| Tokens + pack items + worn gear | Literal | **Worse than dying.** A death leaves a pile you can walk back to; a robbery doesn't. Losing a Motorbike or Canteen ends a session. |
| **Tokens + pack items; worn gear kept** *(rec)* | Hurts badly, recoverable | Water at 0 still risks a thirst spiral (thirst is the top killer) |

The mercy is where the loot goes: the gang's **hideout**, where it can be
taken back (§12). Score is never deducted.

## 6. Counterplay

| Tool | Effect |
|---|---|
| **Settlement** | Full sanctuary: the gang won't enter it or its neighbours |
| **Tunnels** | The gang doesn't go underground. Every hatch connects, so they can't camp them all. Costs AP and bad air. |
| **River + Raft** | The gang can't cross water |
| **Motorbike** (+5 AP) | Outruns any noose — which makes the biker the top-value target |
| **An ally inside the ring** | A witness cancels the snap. Grouping up draws the Doom, so it's a real choice. |
| **The caravan** | Walking with it counts as a witness *(rec)* |
| **The Doom** | The gang scatters from it: lead them into it |
| **Travel light** | Under the value threshold you're never a target |
| **Read the tracks** | Every gang trap sits on gang footprints (§7) |
| **Navigate builds** | Guide and Scout shine on the snare's escape door |

Other rules:
- **Shelter** gives no protection: it's a roof, not a wall.
- **Offline survivors** are never robbed. The ring stays parked for a while,
  so disconnecting doesn't wipe the chase.

## 7. Traps the gang lays

Built on the booby-trap system ([trap-system-spec.md](trap-system-spec.md),
`traps.hpp`). That system currently assumes "no re-arming, no dynamic
placement" (`traps.hpp`, header comment); gang traps break that assumption on
purpose.

**Unchanged:** a gang trap is a forced encounter with the same two doors
(Navigate to back out, Scavenge to work it); escaping leaves it armed and
known only to you; taking anything or springing it spends it; one trap per
hex, never on a Settlement, hatch, POI, impassable hex or an already-trapped
hex. **Only survivors trigger traps** — bandits walk over their own (and
worldgen ones), like the Doom and the caravan. Known traps are *not* walls
for the gang.

### Laying rules

| Knob | Value | Why |
|---|---|---|
| Where | Only the hex a bandit is standing on | The user's rule, and it puts every gang trap on gang footprints |
| Cost | **2 AP** of the bandit's budget | Each trap costs a third of a day's pace: speed vs fencing |
| Kits | 3 per bandit, restocked at the hideout | Bounds a trip |
| Live cap | **9 per gang**; a 10th removes their oldest | Worldgen open ground is ~4% traps; this keeps gang traps local |
| Age-out | 3 dawns | Bandit country moves with the bandits |
| Exclusion | None within 2 of a Settlement or hatch | Otherwise the escape route to sanctuary is the trap |

### Two kinds, by purpose

The gang wants to rob, not kill, so its traps hold you and report you. They
don't maim the way worldgen traps do.

| | **Tripwire** (alarm) | **Snare** |
|---|---|---|
| Laid | While roaming, at chokepoints: fords, passes, river bends, city edges | During stalk and tighten, in the noose's gaps |
| Escape-door fail | The gang **marks you** and gets one free lockstep tighten | **AP −3** *(rec)*, or wiped to 0 on the top tier |
| Work it | 1–2 scrap and their kit (one fewer until they restock) | Same, plus the bait |
| LL damage | Light, with the trap LL floor of 1 | Rare |

- **Tripwires are the gang's sensors.** Its own sense radius is about 5;
  tripwires at chokepoints mark people far beyond that, and should be where
  most marks come from.
- **Stolen-goods bait** *(rec)*: a snare laid while the gang carries robbed
  loot is baited with 1–2 tokens of it. Working it recovers them.

### How traps change the noose

Escape-door risk 20–30 is about a 72–80% pass at skill 1.

- **Fencing the gaps.** A wide noose leaves exactly **3 free exit hexes** on
  ring 2. Snares on them make every way out a ZOC step plus a trap roll.
- **The waiting bandit fences.** Lockstep means the fastest bandit always
  waits for the others; it spends that idle AP walking a gap hex and laying.
  The ring **gets stronger the longer the target lingers**, at no extra cost.
- **Daytime robberies become possible.** A failed snare drains AP, and the
  snap condition is AP-based, so a survivor fresh at dawn can be caught.
- **Still fair:** it takes ignoring three ring warnings (6, 4, 3), stepping
  in the footprints, and failing a roughly 25% roll.

### Tells

- **Footprints.** Every gang trap is on a hex the gang walked. If trail-field
  draws bandit tracks, the rule is "don't step in their footprints". About 1
  tracked hex in 10 is trapped, and tracks fade.
- **Seen it laid.** Laying takes the bandit one world tick, crouched. Any
  survivor with that hex in view gets it in their "knows" mask *(rec)*. This
  deliberately differs from worldgen traps, where watching from next door
  doesn't count.
- The gang knows all its own traps. Other survivors can trip them too, so a
  gang's territory is dangerous to everyone.

## 8. Seeing them coming

With vision radius 1, **a wide noose sits outside the survivor's sight on
most terrain.** Once you're marked:

- **Bandits are drawn regardless of fog**, like the Doom and the caravan.
  Unmarked bandits show only inside vision.
- **They whistle to each other.** Within about 5 hexes the whistle gives
  their bearing; it's a K10 sound cue too.
- **Their tracks show** in trail-field.
- **The K10 counts rings down:** 6, 4, 3.
- **Settlement gossip** (§11).

## 9. Territory and roles

**Predators at the waterhole.** Settlements are where survivors come and go —
dawn rest-healing and the caravan route both run through them — so the gang
is oriented around them.

- **Range:** a cluster of **2–3 neighbouring Settlements** *(rec)*, about 10%
  of the towns, so most of the map stays quiet. Bandits share the range;
  this replaces an earlier draft that gave each bandit a third of the map.
- **Hideout:** Broken Urban **4–8 hexes** from the cluster (Phase 2.95 puts
  ruins against ~70% of Settlements).
- **Migration:** after a robbery, or every ~6 dawns, the gang **lies low** and
  walks together to a new cluster over 1–2 days. Prefer clusters with high
  track heat so it goes where the traffic is. The hideout and hoard **stay
  behind**, which is a raid window.

### The outskirts band

Sanctuary covers the Settlement and its neighbours, and no traps go within 2,
so **the gang works 3–6 hexes out.** On a road between two Settlements 13
apart, the two bands cover everything from hex 3 to hex 10: any trip between
towns in the gang's range crosses bandit country.

Inside the band:
- **Perches:** Ridge, Mountain or Rolling Hills hexes overlooking an approach.
- **Desire lines:** the roads between neighbouring Settlements (the graph the
  caravan already walks), weighted by the Doom's track-heat field so the
  gang watches the paths survivors actually use.
- **Chokepoints:** fords, passes, river bends — where the tripwires go.

### Three roles

Different habits make them tell-apart-able, which is information for the
player.

| Role | AP/day | Habit |
|---|---|---|
| **Lookout** | 6 | Takes ridges, makes most marks; the far slot in a noose |
| **Runner** | 7 | Walks the roads, flanks, cuts off the retreat to town, carries loot |
| **Boss** | 5 | Stays nearest the hideout, takes the closing slot, decides migration |

## 10. Daily routine

| Phase | Behaviour | Tell |
|---|---|---|
| **Dawn** (outbound traffic) | Spread to perches over the exits; mark rich-looking survivors leaving town | Figures on the ridgelines round town: a siege |
| **Day** | Hold perches with short drifts; one walks the road. If someone's marked: *let them leave, then follow* | Fresh tracks, whistles |
| **Dusk** (inbound traffic) | Watch approaches. Inbound survivors have low AP and full packs; the stalker moves to **cut them off** | A bandit between you and home |
| **Night** | Regroup at a camp and light a fire. **No fire means they're hunting**: a hunt runs through the night | A distant campfire — or none |

Add ±3 world ticks of jitter to each phase and skip one now and then, or the
schedule gets timed.

### Movement texture

Every move has a goal the player could guess, executed with natural-looking
imperfection:

- **Correlated walk:** keep heading, turn ±60° occasionally. Meanders, not
  jitter.
- **Terrain preference:** path through cover (Rust Forest, Broken Urban, hill
  shoulders); avoid open Glass Fields; skyline only to perch.
- **Pauses:** 1–4 world ticks to watch or scavenge.
- **Fission and fusion:** split to cover perches, close to 2–5 hexes apart to
  travel, bunch at camp.

### Reactions

- **Storms:** hole up in cover. A storm is the survivor's window to move.
- **The Doom nearby:** scatter, regroup later.
- **Kane's road:** avoided.
- **Player-founded Settlements** extend the network: one inside the range is
  new prey; one outside it is a safe haven you built.

## 11. Settlements as the information hub

Unmarked bandits are hidden by fog, so most of the organic wandering is out of
sight. Put the warnings where players are *(rec)*:

- **Gossip:** resting in a Settlement shows the gang's current range on the
  map with the report's age ("bandits on the Ashford road, 2 days ago"), the
  hoard size (small / fat / bursting), the gang's strength, and any bounty.
- **Wary towns:** a robbery near a town makes it wary for a few days, and the
  rumour spreads to its neighbours. Migrations read through moving rumours.

## 12. Raiding the hideout

### The loop

```
gang robs → hoard grows → towns gossip about it → raids → gang weakens → breaks up → re-forms elsewhere
```

Negative feedback: the more a gang robs, the fatter and more tempting its
hoard, so a gang running riot becomes the map's main draw.

### Hideout state

| Field | Value |
|---|---|
| **Hoard** | Seed ~8 tokens + 1 rare item; every robbery adds the victim's loot; cap about **2–3 packs' worth**; doesn't age while the gang exists |
| **Kits** | Unlaid trap kits; stealing them means fewer snares |
| **Guard distance** | How far the Boss is from the hideout |
| **Strikes** | Successful raids against this gang |

### The raid scene

The hideout hex is a POI. Entering it is an **opt-in** encounter from a new
`hideout` pool, push-your-luck like a trap:

```
approach (Navigate) → stash (Scavenge, bank) → kit store (bank) → Boss's lockbox (top tier, bank)
```

**The Boss's distance is the alarm clock.** The first failed check raises the
alarm; after that you have a set number of rolls before he's back:

| Boss position | Raid |
|---|---|
| At the hideout (camp nights) | Refused: "the fire's lit, they're all home" |
| ≤ 2 hexes | 3 rolls after the alarm |
| 3–5 hexes | 5 rolls |
| > 5, or migrating | No clock; base difficulty only |

Rolls, not real time, because at 5 AP/day the Boss covers about one hex per
~100 s: a real-time return is either instant or never within one scene.

- **Clock runs out: caught.** The normal robbery (everything carried,
  unbanked haul included), LL floor 1, thrown out, **vendetta**.
- **Clean raid** (no alarm): no vendetta.
- The "gang holds still during encounters" rule does not apply here.

### Why it pulls in more players

| Lever | Mechanism |
|---|---|
| **Hoard bigger than one pack** | A full hoard needs 2–3 raiders or repeat trips, and repeats get harder |
| **Heist roles** | **Looter** runs the scene. **Lookout** on an adjacent perch gives +1 roll and can assist through the existing `enc_assist` (−1 difficulty each, max 2). **Decoy**, a well-stocked survivor, gets marked on purpose so the gang's own targeting pulls the Boss away. |
| **One player's hunt is another's opening** | During a noose the Boss is out holding a slot: the hideout is open. Gossip about a hunt is an invitation. |
| **Flat shared credit** | Everyone within 3 hexes, or lookout/decoy that dawn, scores the raid bonus, credited like founding |
| **A visible goal** | The strike track, shown in gossip ("down to two") |
| **Victims know the way** | A robbed survivor gets the hideout in their "knows" mask. No share button: they have to bring friends. Others follow tracks that converge there at dusk. |
| **Settlement bounty** | Breaking the gang pays every survivor who struck it |

### Strike track *(rec)*

| Strikes | Effect |
|---|---|
| 1 | Hoard and kits lost; the Boss stays within 2 of the hideout for 2 days |
| 3 | **The Runner quits: the gang is two.** Two can only close a ring against rivers and crater rims (§1) — the biggest power drop |
| 5 | **The gang breaks up.** World event, bounty paid, the hideout becomes an ordinary ground pile (30-day age-out). A new gang forms at another cluster after ~5 dawns. |

**Vendetta:** for 3 dawns the gang hunts the marked raider first, whatever
they carry *(rec: only raiders caught or who raised the alarm)*. The witness
rule protects groups, so raiders stay together — which draws the Doom.

### Exploit guards

- **Collusion pump** (A gets robbed on purpose, B raids it back): the raid
  score is **flat**, paid **once per gang per dawn**, and robbery costs
  everything for real. Tokens taken back score nothing (same principle as
  death-drop looting).
- **Camping:** one raid scene per survivor per dawn; the hoard only grows
  from robberies.
- **Strikes need a successful bank.** A caught raid doesn't count.

## 13. Pacing targets and metrics

- Ignored, a mark becomes a snap in about **2–3 game days (10–15 min)**:
  ~1.5 days to reach slots plus 4 lockstep tightens at ~2 AP each.
- Per survivor per 2-hour session (~24 days): 2–3 marks, 1–2 rings at ≤ 3,
  **≤ 0.5 snaps** — escapes to snaps near the 1:4 close-call shape already
  targeted for deaths. Tune snaps through the snare's AP drain and kit count,
  not the noose.
- Expect ~1 gang-trap scene per escape attempt that ignores the tracks.
- Group raids should clear ~2× the hoard of solo raids.

Metrics: `mark`, `mark_via_tripwire`, `ring≤3`, `ring2_escape`, `snap`,
`snap_after_snare` (over ~30% of snaps = snare too strong), `bandit_trap_laid`,
`tripped{kind,out}`, `recovered`, `raid{solo|group,out}`, `strike`,
`time_in_town_while_marked`.

## 14. Risks

1. **Robbery → thirst death.** Watch thirst deaths in the 2 days after a snap.
2. **Wounded death spiral.** Less AP is easier prey — the same shape as the
   `(ll-4)/2` term in the encounter difficulty.
3. **"Always end the day in a Settlement"** becomes dominant and shrinks
   exploration. Watch how far bots roam.
4. **Camping a town.** The give-up timer, caravan escort and group exits must
   be real outs.
5. **Raids too easy solo** and the co-op levers stop mattering.
6. **Vendetta spills onto bystanders.** Acceptable if gossip names who
   stirred it up.
7. **Organic polish nobody sees.** Spend it on perches, campfires, tracks and
   gossip, not pathing variety in fog.
8. **`traps.hpp` assumptions.** The tier roll, trap deck and "knows" mask all
   assume worldgen-only placement. A new gang trap must clear the hex's
   "knows" mask, or a survivor who escaped an earlier trap there sees it free.
9. **Mock divergence** (the Doom lesson): the mock must model ZOC costs, the
   AP budget, laying, AP drain, alarm marks, the raid clock and the strike
   track — not plain proximity.
10. **Bots** need a noose danger cost (like Kane's `danger_cost`), an escape
    policy at ring 3, a track-heat cost for gang footprints, and a raid
    policy. No bot uses `enc_assist` yet, so group raids would go unmeasured.
11. **ESP32 cost.** No full A* per tick: precompute the Settlement graph and
    perch list at worldgen and reuse the caravan's greedy step with its
    stuck-limit re-route.
12. **Save state.** Gang positions, budgets, trap metadata (owner, kind, age,
    bait), hoard, kits, strikes, vendettas and the re-form countdown go in a
    sidecar like `kane.bin` / `ghosts.bin`. A gang trap with no sidecar entry
    on load must become something defined; a broken gang's hoard must become
    a ground pile before the save.

## Open decisions

All carry my recommendation; none is confirmed by the user.

| Decision | Recommendation |
|---|---|
| ZOC rule | +1 AP and a 1-token snatch (vs hard walls) |
| Worn gear on a robbery | Kept |
| Loot recoverable at the hideout | Yes |
| An ally inside the ring breaks it | Yes |
| Snare strength | AP −3 (vs wipe to 0) |
| Stolen-goods bait on snares | Yes |
| Bystanders who see a trap laid know it | Yes |
| Gang range | 2–3 Settlements |
| Gossip shows the gang's range | Yes |
| Caravan counts as a witness | Yes |
| Strike track breaks up the gang | Yes |
| Vendetta targets | Raiders caught or who raised the alarm |
| Hoard items keep their owner tag (return-for-score) | No |
