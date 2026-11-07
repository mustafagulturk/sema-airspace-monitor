#!/usr/bin/env python3
"""Generates tests/fixtures/replay_synthetic.jsonl, a SYNTHETIC recording for the tracker.

The cloud CI cannot reach OpenSky, so tracker tests and the replay demo need a
recording that looks like what `sema_collector --record` writes: one OpenSky
/states/all document per line, civil aircraft inside the Turkey polygon only,
one snapshot every ~60-75 s. Real recordings should replace this for the
numbers in the README.

Flights follow waypoints with rate-limited turns (3 deg/s), climbs and
descents, and are sampled the way OpenSky reports them: the newest position
is 0.2-4 s old and its timestamp is truncated to whole seconds. ADS-B position
noise is ~10 m, MLAT ~100 m. On top of that the script injects known events so
tests can check that the tracker sees exactly them:

  * SMX204  signal lost for ~6.5 min mid-cruise   -> one reacquisition
  * SMX310  signal lost for ~17 min               -> track ends, then one break
  * SMX118  one report 1.5 deg north of the track -> one teleport reject
  * SMX512  one report with baro altitude 30 km   -> one bad altitude
  * SMX733  one report with 950 m/s ground speed  -> one impossible speed
  * SMX733  one report with 400 m/s vertical rate -> one bad vertical rate
  * SMX845  from mid-flight on, every position is shifted 2 deg (a duplicate
            transponder address)                   -> one restarted track
  * SMX601  flies two holding laps near Istanbul before landing (turn stress)

Usage: python3 tools/make_replay_fixture.py [--out PATH] [--seed N]
"""

from __future__ import annotations

import argparse
import json
import math
import random
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
T0 = 1762549200  # 2025-11-07 21:00:00 UTC
DURATION = 90 * 60
SIM_DT = 0.5
EARTH_R = 6371008.8

AIRPORTS = {
    "IST": (41.275, 28.752), "SAW": (40.899, 29.309), "ESB": (40.128, 32.995),
    "ADB": (38.292, 27.157), "AYT": (36.899, 30.800), "ADA": (36.982, 35.280),
    "TZX": (40.995, 39.790), "ERZ": (39.956, 41.170), "VAN": (38.468, 43.332),
    "DIY": (37.894, 40.201), "GZT": (36.947, 37.479), "ASR": (38.770, 35.495),
    "SZF": (41.254, 36.555), "BJV": (37.251, 27.664), "DLM": (36.713, 28.792),
    "KYA": (37.979, 32.562), "MLX": (38.435, 38.091), "TEQ": (41.138, 27.919),
}


def load_area():
    doc = json.loads((ROOT / "data" / "turkey_boundary.geojson").read_text())
    geoms = []
    if doc["type"] == "FeatureCollection":
        geoms = [f["geometry"] for f in doc["features"]]
    elif doc["type"] == "Feature":
        geoms = [doc["geometry"]]
    else:
        geoms = [doc]
    polys = []
    for g in geoms:
        if g["type"] == "Polygon":
            polys.append(g["coordinates"])
        elif g["type"] == "MultiPolygon":
            polys.extend(g["coordinates"])
    return polys


def ring_contains(ring, lat, lon):
    inside = False
    j = len(ring) - 1
    for i in range(len(ring)):
        ai_lon, ai_lat = ring[i][0], ring[i][1]
        bj_lon, bj_lat = ring[j][0], ring[j][1]
        if (ai_lat > lat) != (bj_lat > lat):
            x = ai_lon + (lat - ai_lat) * (bj_lon - ai_lon) / (bj_lat - ai_lat)
            if lon < x:
                inside = not inside
        j = i
    return inside


def area_contains(polys, lat, lon):
    for p in polys:
        if ring_contains(p[0], lat, lon) and not any(ring_contains(h, lat, lon) for h in p[1:]):
            return True
    return False


def bearing(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    y = math.sin(lo2 - lo1) * math.cos(la2)
    x = math.cos(la1) * math.sin(la2) - math.sin(la1) * math.cos(la2) * math.cos(lo2 - lo1)
    return math.degrees(math.atan2(y, x)) % 360


def distance(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 2 * EARTH_R * math.asin(min(1.0, math.sqrt(h)))


def move(p, track_deg, d):
    la, lo = math.radians(p[0]), math.radians(p[1])
    th, dr = math.radians(track_deg), d / EARTH_R
    la2 = math.asin(math.sin(la) * math.cos(dr) + math.cos(la) * math.sin(dr) * math.cos(th))
    lo2 = lo + math.atan2(math.sin(th) * math.sin(dr) * math.cos(la), math.cos(dr) - math.sin(la) * math.sin(la2))
    return (math.degrees(la2), math.degrees(lo2))


def offset(p, north_m, east_m):
    return (p[0] + north_m / EARTH_R * 180 / math.pi,
            p[1] + east_m / (EARTH_R * math.cos(math.radians(p[0]))) * 180 / math.pi)


@dataclass
class Sample:
    t: float
    pos: tuple
    alt: float | None  # None on the ground
    speed: float
    track: float
    vrate: float


@dataclass
class Flight:
    icao: str
    callsign: str
    country: str
    waypoints: list
    depart: float
    cruise_alt: float
    cruise_speed: float
    lands: bool = True
    source: int = 0  # 0 ADS-B, 2 MLAT
    samples: list = field(default_factory=list)

    def simulate(self):
        pos = self.waypoints[0]
        wps = list(self.waypoints[1:])
        trk = bearing(pos, wps[0])
        alt, speed, t = 300.0, 85.0, self.depart
        dest = wps[-1]
        on_ground_until = None
        while t < T0 + DURATION + 60:
            if on_ground_until is not None:
                self.samples.append(Sample(t, pos, None, 8.0, trk, 0.0))
                if t > on_ground_until:
                    break
                pos = move(pos, trk, 8.0 * SIM_DT)
                t += SIM_DT
                continue
            target = wps[0]
            if distance(pos, target) < 2500 and len(wps) > 1:
                wps.pop(0)
                target = wps[0]
            dist_dest = distance(pos, dest)
            # Vertical profile: climb, cruise, ~3 deg descent, land.
            if self.lands and dist_dest < (alt - 100) * 22 and len(wps) == 1:
                vrate = -min(9.0, max(3.0, speed * (alt - 50) / max(dist_dest, 1.0)))
                speed_target = 75.0 + min(1.0, alt / 3000) * (self.cruise_speed - 75.0) * 0.7
            elif alt < self.cruise_alt - 30:
                vrate = 12.0 if alt < 6000 else 7.0
                speed_target = self.cruise_speed * (0.6 + 0.4 * min(1.0, alt / self.cruise_alt))
            else:
                vrate = 0.0
                speed_target = self.cruise_speed
            speed += max(-1.0, min(1.0, speed_target - speed)) * SIM_DT
            alt = max(0.0, alt + vrate * SIM_DT)
            want = bearing(pos, target)
            diff = (want - trk + 540) % 360 - 180
            trk = (trk + max(-3.0 * SIM_DT, min(3.0 * SIM_DT, diff))) % 360
            self.samples.append(Sample(t, pos, alt, speed, trk, vrate))
            pos = move(pos, trk, speed * SIM_DT)
            t += SIM_DT
            if self.lands and len(wps) == 1 and dist_dest < 2500 and alt < 120:
                on_ground_until = t + 150
            if not self.lands and len(wps) == 1 and dist_dest < 2500:
                break

    def at(self, t):
        if not self.samples or t < self.samples[0].t or t > self.samples[-1].t:
            return None
        i = min(int((t - self.samples[0].t) / SIM_DT), len(self.samples) - 1)
        s = self.samples[i]
        f = (t - s.t) / SIM_DT
        if i + 1 < len(self.samples) and 0 <= f <= 1:
            n = self.samples[i + 1]
            pos = (s.pos[0] + (n.pos[0] - s.pos[0]) * f, s.pos[1] + (n.pos[1] - s.pos[1]) * f)
            alt = None if s.alt is None or n.alt is None else s.alt + (n.alt - s.alt) * f
            return Sample(t, pos, alt, s.speed, s.track, s.vrate)
        return s


def make_flights(rng):
    flights = []
    names = list(AIRPORTS)
    used = set()

    def icao():
        while True:
            a = f"4b{rng.randrange(0x8000, 0xffff):04x}"
            if a not in used:
                used.add(a)
                return a

    def add(cs, route, depart, **kw):
        wps = [AIRPORTS[r] if isinstance(r, str) else r for r in route]
        flights.append(Flight(icao(), cs, kw.pop("country", "Turkey"), wps, depart,
                              kw.pop("cruise_alt", rng.choice([9450.0, 10060.0, 10670.0, 11280.0])),
                              kw.pop("cruise_speed", rng.uniform(215, 245)), **kw))

    # Scripted flights carrying the injected events.
    add("SMX204", ["IST", "VAN"], T0 - 1500)
    add("SMX310", ["ADB", "TZX"], T0 - 900)
    add("SMX118", ["ESB", "ADA"], T0 + 300)
    add("SMX512", ["AYT", "SZF"], T0 - 600)
    add("SMX733", ["DIY", "SAW"], T0 - 1200)
    add("SMX845", ["ERZ", "IST"], T0 - 1800)
    hold_c = (41.55, 28.45)
    hold = [offset(hold_c, 0, -9000), offset(hold_c, 0, 9000), offset(hold_c, -7000, 9000),
            offset(hold_c, -7000, -9000)]
    add("SMX601", ["ADA", offset(hold_c, 0, -9000)] + hold + hold + ["IST"], T0 - 2700)
    # Overflights entering from the west and leaving east/south-east.
    add("SMX901", [(41.6, 25.3), (40.2, 35.0), (38.4, 45.2)], T0 - 3000, lands=False, country="Germany",
        cruise_alt=11280.0)
    add("SMX902", [(40.4, 25.2), (37.6, 36.0), (36.4, 37.5), (35.0, 38.5)], T0 - 1500, lands=False,
        country="Netherlands", cruise_alt=10670.0)
    # Background domestic traffic, some with MLAT positions.
    for k in range(34):
        a, b = rng.sample(names, 2)
        while distance(AIRPORTS[a], AIRPORTS[b]) < 250_000:
            a, b = rng.sample(names, 2)
        add(f"SMY{k + 1:03d}", [a, b],
            T0 + rng.uniform(-4000, 3600), source=2 if k % 9 == 4 else 0)
    for f in flights:
        f.simulate()
    return flights


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "tests" / "fixtures" / "replay_synthetic.jsonl"))
    ap.add_argument("--seed", type=int, default=20261107)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    area = load_area()
    flights = make_flights(rng)

    gaps = {"SMX204": (T0 + 1800, T0 + 1800 + 390), "SMX310": (T0 + 1500, T0 + 1500 + 1020)}
    snap_times = []
    t = T0
    while t <= T0 + DURATION:
        snap_times.append(t)
        t += rng.randint(62, 75)

    # One-off glitches go into the snapshot nearest to these times.
    def nearest(target):
        return min(snap_times, key=lambda s: abs(s - target))

    glitch = {
        ("SMX118", nearest(T0 + 2400)): "teleport",
        ("SMX512", nearest(T0 + 1200)): "bad_alt",
        ("SMX733", nearest(T0 + 600)): "bad_speed",
        ("SMX733", nearest(T0 + 1300)): "bad_vrate",
    }
    dup_from = T0 + 2100  # SMX845 address duplicated from here on
    last_fix = {}

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
            sigma = 100.0 if f.source == 2 else 10.0
            pos = offset(s.pos, rng.gauss(0, sigma), rng.gauss(0, sigma))
            kind = glitch.get((f.callsign, ts))
            if f.callsign == "SMX845" and ts >= dup_from:
                pos = (pos[0] - 2.0, pos[1] + 0.5)
            if kind == "teleport":
                pos = (pos[0] + 1.5, pos[1])
            if not area_contains(area, *pos) and kind != "teleport":
                continue
            on_ground = s.alt is None
            baro = None if on_ground else round(s.alt / 7.62) * 7.62
            speed = s.speed + rng.gauss(0, 0.5)
            track = (s.track + rng.gauss(0, 0.3)) % 360
            vrate = round(s.vrate / 0.325) * 0.325
            if kind == "bad_alt":
                baro = 30000.0
            if kind == "bad_speed":
                speed = 950.0
            if kind == "bad_vrate":
                vrate = 400.0
            tpos = int(math.floor(ts - lag))
            last_fix[f.icao] = tpos
            rows.append([
                f.icao, f"{f.callsign:<8}", f.country, tpos, int(ts - rng.uniform(0, 0.9)),
                round(pos[1], 5), round(pos[0], 5), None if baro is None else round(baro, 2), on_ground,
                round(speed, 2), round(track, 2), None if on_ground else round(vrate, 2), None,
                None if baro is None else round(baro + 120 + rng.gauss(0, 5), 2), "2000", False, f.source,
            ])
        lines.append(json.dumps({"time": ts, "states": rows}, separators=(",", ":")))

    Path(args.out).write_text("\n".join(lines) + "\n")
    print(f"wrote {len(lines)} snapshots, {sum(len(json.loads(x)['states']) for x in lines)} rows to {args.out}")


if __name__ == "__main__":
    main()
