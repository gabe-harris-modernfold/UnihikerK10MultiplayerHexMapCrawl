"""Every quest chain, played end to end by a real quester against the mock.

On the board a quest step is found by opening POIs -- the map never says
which file is on a hex -- so an arena run reaches whatever the map deals it,
in whatever order.  This stages each chain on purpose instead: the quester's
own socket sends the mock's `dbg_enc` for each step (it opens a scene for
its sender), and the quester's own policy plays it, exactly as it would on
the board.  `dbg_force` keeps the rolls winning unless --real-rolls, because
the question here is whether the chain holds together, not whether a 55%
check comes up.

  jack       Jack's Chopper in order: accept, the mask, the hand-over
  ungated    the hand-over again WITHOUT the mask, from a fresh journal --
             it should be refused; quest_ungated says it was not
  sticky     the Sticky Note, then the Dial-Up Cult's password and the Router
  peram      the Perambulator on all three terrains: items, then the gates
             they open (and every gate probed once without its item first)
  workshop   settlement/15-21: every recipe taught, then (on a Settlement
             the mock makes with dbg_settle) every one crafted
  items      a heal, litter, a ration: the item layer on its own

Machine Gun Kane is kane_check.py (`--policies quester,...` plays it).
Mock only.  On the board run an arena with questers in the fleet and read
the quest ledger it prints.

    python quest_check.py [host[:port]] [--parts jack,ungated,...] [--real-rolls]

Every check prints ok/FAIL; the exit code is the number of failures.
"""
import argparse
import asyncio
import random
import sys
import time
from datetime import datetime
from pathlib import Path

import config
import items as IT
import policy as policy_mod
import quests as QM
from client import BotClient, RateLimiter, SlotBroker
from findings import FindingLog
from policy.base import Action
from record import Recorder

RUNS_DIR = Path(__file__).resolve().parent / "runs"
fails = 0


def check(label, cond, detail=""):
    global fails
    if cond:
        print(f"  ok   {label}")
    else:
        fails += 1
        print(f"  FAIL {label}" + (f"  ({detail})" if detail else ""))


async def wait_until(pred, timeout, every=0.3):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if pred():
            return True
        await asyncio.sleep(every)
    return pred()


class Driver:
    """Stages scenes on the quester's own seat and keeps it alive."""

    def __init__(self, bot: BotClient, force: bool):
        self.b = bot
        self.force = force

    @property
    def me(self):
        return self.b.obs.me

    @property
    def pol(self):
        return self.b.policy

    async def send(self, msg):
        await self.b._send(msg)

    def holds(self, iid) -> bool:
        return iid in self.me.equip or any(t == iid and q > 0 for t, q in
                                          zip(self.me.inv_type, self.me.inv_qty))

    async def tend(self):
        """The mock's survivors live on the edge; a staged quest should not
        end in a death that has nothing to do with it."""
        if self.me.ll < 5:
            await self.send({"t": "dbg_hurt", "ll": 7, "wnd": [0, 0]})
        # Staples for the dawns, and the tokens the scenes charge for a
        # choice: a lesson behind a 1-scrap question is not planned at all
        # when the pack holds no scrap (can_pay), and the quester banks
        # instead -- settlement/17's Static Chew went untaught that way.
        floor = (6, 4, 1, 2, 3)
        if any(have < need for have, need in zip(self.me.inv, floor)):
            inv = [max(have, need) for have, need in zip(self.me.inv, floor)]
            await self.send({"t": "dbg_setinv", "inv": inv})
        await asyncio.sleep(0.4)

    async def idle(self):
        return await wait_until(lambda: self.b.obs.encounter is None
                                and not self.me.in_encounter, 30)

    async def scene(self, biome, eid, timeout=90.0) -> bool:
        """Open biome/eid for the quester and wait until the scene is over."""
        await self.idle()
        await self.tend()
        # dbg_force is one roll's worth, and the quester rolls again 0.35 s
        # after each result: armed on a 0.8 s timer it missed every other
        # roll, and a failed check with an ending hazard lost the recipes
        # already won (settlement/17 taught 8 and 9, then forfeited them).
        # Arm it before the scene opens and again the moment each roll lands,
        # ahead of the bot's next choice on the same socket.
        if self.force:
            await self.send({"t": "dbg_force", "out": 1})
        await self.send({"t": "dbg_enc", "biome": biome, "id": eid})
        if not await wait_until(lambda: self.b.obs.encounter is not None, 10):
            return False
        t0 = time.monotonic()
        rolls = self.pol.stats["rolls"]
        while time.monotonic() - t0 < timeout:
            if self.b.obs.encounter is None:
                await asyncio.sleep(1.0)            # the bank's item_result
                return True
            if self.force and self.pol.stats["rolls"] != rolls:
                rolls = self.pol.stats["rolls"]
                await self.send({"t": "dbg_force", "out": 1})
            await asyncio.sleep(0.1)
        return False

    async def take(self, iid):
        """Remove every copy of `iid`: off the body first, then the pack."""
        if iid in self.me.equip:
            await self.send({"t": "unequip_item", "eslot": self.me.equip.index(iid)})
            await asyncio.sleep(0.8)
        for s, t in enumerate(self.me.inv_type):
            if t == iid:
                await self.send({"t": "dbg_setitem", "slot": s, "item": 0, "qty": 0})
        await asyncio.sleep(0.6)

    async def give(self, iid, qty=1):
        free = [s for s, t in enumerate(self.me.inv_type[:self.me.inv_slots]) if not t]
        if free:
            await self.send({"t": "dbg_setitem", "slot": free[0], "item": iid, "qty": qty})
            await asyncio.sleep(0.6)

    def fresh_journal(self):
        """A new survivor's view of the quests, keeping the oracle hookup."""
        old = self.pol.journal
        j = QM.QuestJournal()
        j.flag, j.who = old.flag, old.who
        self.pol.journal = j
        return j


def found(findings, check_name) -> int:
    return sum(f["count"] for f in findings.summary() if f["check"] == check_name)


async def part_jack(d: Driver, findings):
    print("jack's chopper, in order")
    for iid in (33, 26):
        await d.take(iid)
    j = d.fresh_journal()
    ok = await d.scene("scrub", 19)
    check("scrub/19 played to the end", ok)
    check("the quester agreed to fetch the mask (jack.accept)", "jack.accept" in j.done,
          f"done={sorted(j.done)}")
    for attempt in range(4):        # drop_intimidate_mask pays 85% of the time
        ok = await d.scene("urban", 19)
        if d.holds(33):
            break
    check("the gas station paid the Intimidate Mask", d.holds(33), f"after {attempt + 1} visit(s)")
    check("jack.mask done", "jack.mask" in j.done)
    before = found(findings, "quest_ungated")
    ok = await d.scene("scrub", 20)
    await wait_until(lambda: d.holds(26), 5)
    check("handing the mask over paid the Motorbike", d.holds(26))
    check("jack.deliver done, in order, with no finding",
          "jack.deliver" in j.done and found(findings, "quest_ungated") == before)


async def part_ungated(d: Driver, findings):
    print("jack's hand-over without the mask (the sequencing check)")
    for iid in (33, 26):
        await d.take(iid)
    d.fresh_journal()
    before = found(findings, "quest_ungated")
    await d.scene("scrub", 20)
    got = found(findings, "quest_ungated") - before
    # This is the content defect quests.lint() reports: the check passes when
    # the oracle catches it, and says so either way.
    check("the oracle flags a hand-over without the mask (quest_ungated)", got > 0)
    print(f"       the step {'WENT THROUGH' if got else 'was refused'} without the mask; "
          f"Motorbike paid: {d.holds(26)}")


async def part_sticky(d: Driver, findings):
    print("the sticky note and the dial-up cult")
    for iid in (51, 38):
        await d.take(iid)
    j = d.fresh_journal()
    await d.scene("scrub", 18)
    check("the desk jockey paid the Sticky Note", d.holds(51))
    # Counted before the scene: the bot reads the map on the first cycle
    # after the bank, which is sooner than scene() returns.
    r0 = d.pol.stats.get("reveals", 0)
    await d.scene("urban", 15)
    check("the quester presented the note and beat the Router (sticky.cult)",
          "sticky.cult" in j.done, f"done={sorted(j.done)}")
    check("and the Pre-War Net Map is in the pack", d.holds(38))
    await wait_until(lambda: d.pol.stats.get("reveals", 0) > r0, 30)
    check("which it reads (the whole map)", d.pol.stats.get("reveals", 0) > r0)


async def part_peram(d: Driver, findings):
    print("the perambulator (null meridian): three visits")
    for iid in (66, 67, 68, 69):
        await d.take(iid)
    j = d.fresh_journal()
    QM.LEDGER.gates_probed.clear()
    for biome, eid in (("scrub", 21), ("glass", 3), ("ridge", 3)):
        ok = await d.scene(biome, eid)
        held = [i for i in (66, 67, 68, 69) if d.holds(i)]
        worn = [i for i in (66, 67, 68, 69) if i in d.me.equip]
        print(f"       {biome}/{eid}: {'ok' if ok else 'TIMEOUT'} -- holding {held}, wearing {worn}, "
              f"steps {sorted(k for k in j.done if k.startswith('perambulator'))}")
        await asyncio.sleep(2.0)            # gear_action: the glove goes on
    got = [i for i in (66, 67, 68, 69) if d.holds(i)]
    used = [k for k in j.done if k.startswith("perambulator.") and ".get" not in k]
    check("it collected Null Meridian items", len(got) >= 3, f"holding {got}")
    check("and used them on the gates they open", len(used) >= 2, f"gates {used}")
    probes = dict(QM.LEDGER.gates_probed)
    check("every gate it lacked the item for was probed and refused",
          probes and all(v.startswith("refused") for v in probes.values()), str(probes))
    check("no gate let it through without the item", found(findings, "gate_bypassed") == 0)


async def part_workshop(d: Driver, findings):
    print("the workshop: settlement/15-21")
    j = d.fresh_journal()
    for eid in range(15, 22):
        await d.scene("settlement", eid)
    learned = [r for r in range(1, 18) if d.me.known_recipes & (1 << (r - 1))]
    check("every recipe 1-17 is known after the lessons", len(learned) == 17,
          f"known {learned}")
    check("the journal booked every lesson",
          all(f"recipe.{r}.learn" in j.done for r in range(1, 18)))
    # Now make each one, on a Settlement of our own, with the inputs to hand.
    await d.send({"t": "dbg_settle"})
    await asyncio.sleep(1.0)
    made = set()
    for rid in range(1, 18):
        rec = config.RECIPES.get(rid)
        if rec is None:
            continue
        await d.idle()
        await d.tend()
        # The materials land one send at a time, and a recipe it has already
        # made that shares one of them (the Nostril Salts took the Static
        # Chew's seed) is crafted in the gap.  Stage with the workshop shut.
        d.pol.craft_extras = False
        for s in range(d.me.inv_slots):     # an empty pack: room for the result
            if d.me.inv_type[s]:
                await d.send({"t": "dbg_setitem", "slot": s, "item": 0, "qty": 0})
        await asyncio.sleep(0.6)
        for slot, (item, qty) in enumerate(rec["mats"]):
            await d.send({"t": "dbg_setitem", "slot": slot, "item": item, "qty": qty})
        inv = [max(8, c + 6) if i < 2 else c + 3 for i, c in enumerate(rec["cost"])]
        await d.send({"t": "dbg_setinv", "inv": inv})
        await d.send({"t": "dbg_hurt", "ll": 7, "wnd": [0, 0]})
        await asyncio.sleep(0.6)
        d.pol.craft_extras = True
        if await wait_until(lambda: f"recipe.{rid}.craft" in j.done, 25):
            made.add(rid)
    check("and crafted every one of them at the Settlement", made == set(range(1, 18)),
          f"missing {sorted(set(range(1, 18)) - made)}")


async def part_items(d: Driver, findings):
    print("items")
    await d.idle()
    for s in range(d.me.inv_slots):
        if d.me.inv_type[s]:
            await d.send({"t": "dbg_setitem", "slot": s, "item": 0, "qty": 0})
    await d.send({"t": "dbg_hurt", "ll": 3, "wnd": [0, 1]})
    await asyncio.sleep(0.6)
    # Counted before the items land: the kit is used on the first cycle
    # after it appears, inside give()'s own settling sleep.
    used0 = dict(d.pol.stats["items_used"])
    await d.give(9)
    await d.give(1)
    await wait_until(lambda: d.pol.stats["items_used"].get(9, 0) > used0.get(9, 0), 15)
    check("a major wound: it used the Anti-Rot Kit", d.pol.stats["items_used"].get(9, 0) > used0.get(9, 0))
    await wait_until(lambda: d.pol.stats["items_used"].get(1, 0) > used0.get(1, 0), 15)
    check("LL 3 of 7: it used the Trauma Patch", d.pol.stats["items_used"].get(1, 0) > used0.get(1, 0))
    for s in range(d.me.inv_slots):
        await d.send({"t": "dbg_setitem", "slot": s, "item": 49, "qty": 1})
    d0 = d.pol.stats["junk_dropped"]
    await wait_until(lambda: d.pol.stats["junk_dropped"] > d0, 15)
    check("a pack full of Crater Deeds: it drops one", d.pol.stats["junk_dropped"] > d0)


PARTS = {"jack": part_jack, "ungated": part_ungated, "sticky": part_sticky,
         "peram": part_peram, "workshop": part_workshop, "items": part_items}


async def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", default="localhost:8765")
    ap.add_argument("--parts", default=",".join(PARTS))
    ap.add_argument("--real-rolls", action="store_true",
                    help="do not force the rolls (a failed check ends the step early)")
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args(argv)
    parts = [p.strip() for p in args.parts.split(",") if p.strip()]

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = RUNS_DIR / f"quests-{stamp}.jsonl"
    rng = random.Random(args.seed)
    QM.LEDGER.reset()
    with Recorder(path, {"kind": "quest_check", "host": args.host, "parts": parts}) as rec:
        findings = FindingLog(rec)
        pol = policy_mod.make("quester", random.Random(args.seed))
        # Staged, not found: it plays only the scenes this script opens, and
        # stays where it is between them (pursuit would open the mock's own
        # POIs, out of order, and Kane is kane_check.py's).
        pol.pursue = lambda obs: Action("noop", why="quest_check: staged scenes only")
        pol.engage_encounters = False
        pol.kane_quest = False
        pol.shop = False
        bot = BotClient(host=args.host, preferred_arch=0, policy=pol, recorder=rec,
                        rng=rng, limiter=RateLimiter(0.25, rng),
                        global_limiter=RateLimiter(0.10, rng, jitter=0.25),
                        broker=SlotBroker(), findings=findings)
        task = asyncio.create_task(bot.run())
        try:
            check("quester seated", await wait_until(bot.joined, 30), bot.label)
            if not bot.joined():
                return
            await asyncio.sleep(2.0)
            d = Driver(bot, force=not args.real_rolls)
            for name in parts:
                await PARTS[name](d, findings)
            print("ledger")
            for line in QM.LEDGER.report(QM.catalog()):
                print(line)
            c = pol.content_score()
            print(f"    items used {c['items_used']} dropped {c['junk_dropped']} "
                  f"crafted {c['crafted_extra']} probes {c['gates_probed']}")
            findings.print_summary()
        finally:
            bot.stop.set()
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        print(f"log: {path}")
    print(f"{fails} failure(s)")


if __name__ == "__main__":
    asyncio.run(main())
    sys.exit(min(fails, 100))
