#!/usr/bin/env python3
"""Generates tests/fixtures/anomaly_synthetic.jsonl, a SYNTHETIC recording for anomaly detection.

Same format and flight model as tools/make_replay_fixture.py (whose helpers it
reuses), with a different seed and one scripted aircraft per anomaly rule, so
tests and the `sema_replay --events` demo can check that exactly these are
found and nothing else:

  * SMA700  squawks 7700 for ~12 min in cruise           -> one emergency
  * SMA701  one single report with squawk 7500           -> nothing (one corrupt reply)
  * SMA350  descends at ~35 m/s from cruise to ~3,500 m  -> one rapid descent
  * SMA180  turns back to its origin at cruise           -> one route deviation
  * SMA601  flies three racetrack laps before landing     -> one holding
  * SMA090  silent for ~9 min mid-cruise over busy airspace -> one signal loss
            (only with a low coverage threshold: 90 minutes of data cannot
            reach the 24-hour default; tests and the demo pass a lower one,
            e.g. `sema_replay --events --loss-min-coverage 3`)

The background traffic (ordinary domestic flights) must raise nothing. The
aircraft, addresses and callsigns are made up; this is not OpenSky data.

Usage: python3 tools/make_anomaly_fixture.py [--out PATH] [--seed N]
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_replay_fixture as base  # noqa: E402

T0 = base.T0
DURATION = base.DURATION
AIRPORTS = base.AIRPORTS


def make_flights(rng):
    flights = []
    used = set()

    def icao():
        while True:
            a = f"4b{rng.randrange(0x8000, 0xffff):04x}"
            if a not in used:
                used.add(a)
                return a

    def add(cs, route, depart, **kw):
        wps = [AIRPORTS[r] if isinstance(r, str) else r for r in route]
        f = base.Flight(icao(), cs, kw.pop("country", "Turkey"), wps, depart,
                        kw.pop("cruise_alt", rng.choice([9450.0, 10060.0, 10670.0, 11280.0])),
                        kw.pop("cruise_speed", rng.uniform(215, 245)), **kw)
        flights.append(f)
        return f

    add("SMA700", ["ESB", "VAN"], T0 - 1800)
    add("SMA701", ["ADB", "ESB"], T0 - 1200)
    add("SMA350", ["IST", "DIY"], T0 - 2400, cruise_alt=11280.0)
    # Out towards Erzurum and back to Ankara: the turn-back happens at cruise.
    turn = base.move(AIRPORTS["ESB"], base.bearing(AIRPORTS["ESB"], AIRPORTS["ERZ"]), 420_000)
    add("SMA180", ["ESB", turn, "ESB"], T0 - 600, cruise_alt=10670.0)
    hold_c = (40.30, 33.25)  # north-east of Ankara
    hold = [base.offset(hold_c, 0, -9000), base.offset(hold_c, 0, 9000), base.offset(hold_c, -7000, 9000),
            base.offset(hold_c, -7000, -9000)]
    add("SMA601", ["TZX", base.offset(hold_c, 0, -9000)] + hold * 3 + ["ESB"], T0 - 3600, cruise_alt=6000.0,
        cruise_speed=140.0)
    add("SMA090", ["IST", "VAN"], T0 - 1500)
    # A second aircraft on the same airway, so the cells along it are covered.
    add("SMA091", ["IST", "VAN"], T0 - 1900)
    add("SMA092", ["SAW", "VAN"], T0 - 1300)

    names = list(AIRPORTS)
    for k in range(30):
        a, b = rng.sample(names, 2)
        while base.distance(AIRPORTS[a], AIRPORTS[b]) < 250_000:
            a, b = rng.sample(names, 2)
        add(f"SMB{k + 1:03d}", [a, b], T0 + rng.uniform(-4000, 3600))
    for f in flights:
        f.simulate()

    # Rapid descent: from T0+1500, down at 35 m/s to 3,500 m, then level.
    f = next(x for x in flights if x.callsign == "SMA350")
    start = T0 + 1500
    for s in f.samples:
        if s.t >= start and s.alt is not None:
            s.alt = max(3500.0, s.alt - 35.0 * (s.t - start))
            s.vrate = -35.0 if s.alt > 3500.0 else 0.0
    return flights


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(base.ROOT / "tests" / "fixtures" / "anomaly_synthetic.jsonl"))
    ap.add_argument("--seed", type=int, default=20261108)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    area = base.load_area()
    flights = make_flights(rng)

    gaps = {"SMA090": (T0 + 1500, T0 + 1500 + 540)}
    snap_times = []
    t = T0
    while t <= T0 + DURATION:
        snap_times.append(t)
        t += rng.randint(62, 75)

    def nearest(target):
        return min(snap_times, key=lambda s: abs(s - target))

    squawk = {("SMA701", nearest(T0 + 1800)): "7500"}
    for ts in snap_times:
        if T0 + 1200 <= ts <= T0 + 1200 + 720:
            squawk[("SMA700", ts)] = "7700"

    lines = []
    for ts in snap_times:
        rows = []
        for f in flights:
            if f.callsign in gaps and gaps[f.callsign][0] <= ts < gaps[f.callsign][1]:
                continue
            lag = rng.uniform(0.2, 4.0)
            s = f.at(ts - lag)
            if s is None:
                continue
            pos = base.offset(s.pos, rng.gauss(0, 10.0), rng.gauss(0, 10.0))
            if not base.area_contains(area, *pos):
                continue
            on_ground = s.alt is None
            baro = None if on_ground else round(s.alt / 7.62) * 7.62
            tpos = int(math.floor(ts - lag))
            rows.append([
                f.icao, f"{f.callsign:<8}", f.country, tpos, int(ts - rng.uniform(0, 0.9)),
                round(pos[1], 5), round(pos[0], 5), None if baro is None else round(baro, 2), on_ground,
                round(s.speed + rng.gauss(0, 0.5), 2), round((s.track + rng.gauss(0, 0.3)) % 360, 2),
                None if on_ground else round(round(s.vrate / 0.325) * 0.325, 2), None,
                None if baro is None else round(baro + 120 + rng.gauss(0, 5), 2),
                squawk.get((f.callsign, ts), "2000"), False, 0,
            ])
        lines.append(json.dumps({"time": ts, "states": rows}, separators=(",", ":")))

    Path(args.out).write_text("\n".join(lines) + "\n")
    print(f"wrote {len(lines)} snapshots, {sum(len(json.loads(x)['states']) for x in lines)} rows to {args.out}")


if __name__ == "__main__":
    main()
