#!/usr/bin/env python3
"""openTTD-HSB MCP server — let an AI agent build OpenTTD scenarios.

Thin wrappers over scripts/: the agent authors data files (cities CSV,
industries/rivers JSON), builds heightmap + towns + manifest, validates,
and runs the headless builder. Script failures are returned as
``{"ok": False, ...}`` with the script output attached, so the agent can
read diagnostics and iterate.

Transport: stdio (``python3 mcp/server.py``). See mcp/README.md.

Bounds MUST agree across every step — they define one shared lat/lon ->
tile mapping (see docs/FORMATS.md, resource ``hsb://formats``).

This file is part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
from __future__ import annotations

import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Optional

from pydantic import BaseModel, Field

try:
    from mcp.server.mcpserver import MCPServer
except ImportError:  # pragma: no cover - import-time hint for missing dep
    print("mcp/server.py needs the 'mcp' package (mcp/requirements.txt)",
          file=sys.stderr)
    raise

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"


# ---------------------------------------------------------------- models

class Bounds(BaseModel):
    """Geographic bounds shared by heightmap, towns, industries, rivers."""
    lat_min: float = Field(description="Southern edge, degrees north")
    lat_max: float = Field(description="Northern edge, degrees north")
    lon_min: float = Field(description="Western edge, degrees east")
    lon_max: float = Field(description="Eastern edge, degrees east")

    def check(self) -> None:
        if not (self.lat_min < self.lat_max and self.lon_min < self.lon_max):
            raise ValueError(
                "bounds must satisfy lat_min < lat_max and lon_min < lon_max, "
                f"got {self.model_dump()}")

    def argv(self) -> list[str]:
        self.check()
        return ["--lat-min", str(self.lat_min), "--lat-max", str(self.lat_max),
                "--lon-min", str(self.lon_min), "--lon-max", str(self.lon_max)]


class CityRow(BaseModel):
    """One row of the cities.csv input for build-towns."""
    name: str
    lat: float
    lon: float
    pop_real: int = Field(description="Real inhabitants (pre-scaling)")
    city: bool = Field(default=False,
                       description="True for OpenTTD cities (large metros)")
    source: str = Field(default="estimate",
                        description="Provenance label, e.g. destatis2023")


class GeocodeQuery(BaseModel):
    """One town name to geocode via Nominatim."""
    name: str
    pop: int = Field(description="Real inhabitants (pre-scaling)")


class IndustryPlacement(BaseModel):
    """One entry of industries.json."""
    type: str = Field(description="In-game industry display name, "
                                  "e.g. 'Farm', 'Power Plant' (NewGRF-dependent)")
    lat: float
    lon: float
    near: str = Field(default="",
                      description="Free-form label for logs, e.g. town name")


class RiverSegment(BaseModel):
    """One entry of rivers.json."""
    name: str
    kind: str = Field(default="river",
                      description="'river' (inland) or 'sea' (tidal, ports work)")
    width: int = Field(default=1, ge=1, le=3,
                       description="Brush width in tiles at 4096")
    waypoints: list[tuple[float, float]] = Field(
        description="Polyline as [[lat, lon], ...]")


# ---------------------------------------------------------------- helpers

def _abspath(p: str) -> str:
    if not p or "\x00" in p:
        raise ValueError(f"bad path: {p!r}")
    return os.path.abspath(os.path.expanduser(p))


def _run(script: str, argv: list[str], timeout: int) -> dict[str, Any]:
    """Run a scripts/ helper, capture output. Never raises on script failure."""
    path = SCRIPTS / script
    if not path.exists():
        return {"ok": False, "error": f"script missing: {path}"}
    try:
        proc = subprocess.run(
            [sys.executable, str(path), *argv],
            capture_output=True, text=True, timeout=timeout,
            cwd=str(ROOT),
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"{script} timed out after {timeout}s"}
    return {
        "ok": proc.returncode == 0,
        "returncode": proc.returncode,
        "output": (proc.stdout + proc.stderr)[-8000:],
    }


# ------------------------------------------------------- plain operations
# (importable + testable without MCP transport; tools delegate to these)

def do_build_heightmap(bounds: Bounds, zoom: int = 8, size: int = 2048,
                       out: str = "heightmap.png",
                       master_out: str = "heightmap-master.tif",
                       timeout: int = 1200) -> dict[str, Any]:
    res = _run("build-heightmap.py", [
        *bounds.argv(), "--zoom", str(zoom), "--size", str(size),
        "--out", _abspath(out), "--master-out", _abspath(master_out),
    ], timeout)
    if res["ok"]:
        res.update({"heightmap": _abspath(out), "master": _abspath(master_out)})
    return res


def do_write_cities_csv(rows: list[CityRow], path: str) -> dict[str, Any]:
    rows = list(rows)
    if not rows:
        return {"ok": False, "error": "no rows given"}
    path = _abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "lat", "lon", "pop_real", "city", "source"])
        for r in rows:
            w.writerow([r.name, r.lat, r.lon, r.pop_real,
                        "1" if r.city else "0", r.source])
    return {"ok": True, "cities": path, "rows": len(rows)}


def do_geocode(queries: list[GeocodeQuery], suffix: str = "",
               timeout: int = 600) -> dict[str, Any]:
    """Geocode town names; returns CSV-ready rows (Nominatim, ~1s per name)."""
    queries = list(queries)
    if not queries:
        return {"ok": False, "error": "no queries given"}
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False,
                                     encoding="utf-8") as f:
        for q in queries:
            f.write(f"{q.name},{q.pop}\n")
        names_path = f.name
    argv = ["--geocode", names_path, "--cities", "x", "--heightmap", "x",
            "--lat-min", "0", "--lat-max", "1", "--lon-min", "0",
            "--lon-max", "1"]
    if suffix:
        argv += ["--geocode-suffix", suffix]
    # --geocode returns before bounds are used; dummies satisfy argparse.
    res = _run("build-towns.py", argv, timeout)
    os.unlink(names_path)
    rows: list[dict[str, Any]] = []
    failed: list[str] = []
    for line in res.get("output", "").splitlines():
        line = line.strip()
        if line.startswith("# FAILED"):
            failed.append(line)
        elif line and not line.startswith("["):
            try:
                name, lat, lon, pop, city, source = line.split(",")
                rows.append({"name": name, "lat": float(lat),
                             "lon": float(lon), "pop_real": int(pop),
                             "city": city == "1", "source": source})
            except ValueError:
                pass
    res.update({"rows": rows, "failed": failed, "ok": not failed})
    return res


def do_build_towns(cities: str, heightmap: str, bounds: Bounds,
                   out: str = "towns.json",
                   debug_out: str = "towns.debug.csv",
                   pop_divisor: float = 450.0, pop_min: int = 300,
                   nudge_px: int = 4, sample_only: bool = False,
                   strict: bool = False,
                   timeout: int = 600) -> dict[str, Any]:
    argv = ["--cities", _abspath(cities), "--heightmap", _abspath(heightmap),
            *bounds.argv(), "--out", _abspath(out),
            "--debug-out", _abspath(debug_out),
            "--pop-divisor", str(pop_divisor), "--pop-min", str(pop_min),
            "--nudge-px", str(nudge_px)]
    if sample_only:
        argv.append("--sample")
    if strict:
        argv.append("--strict")
    res = _run("build-towns.py", argv, timeout)
    if res["ok"]:
        try:
            towns = json.load(open(_abspath(out), encoding="utf-8"))
            res.update({"towns": _abspath(out), "debug": _abspath(debug_out),
                        "count": len(towns),
                        "cities": sum(1 for t in towns if t.get("city"))})
        except OSError as e:
            res.update({"ok": False, "error": str(e)})
    return res


def do_write_industries(entries: list[IndustryPlacement],
                        path: str) -> dict[str, Any]:
    entries = list(entries)
    if not entries:
        return {"ok": False, "error": "no entries given"}
    path = _abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = [{"type": e.type, "lat": e.lat, "lon": e.lon, "near": e.near or e.type}
            for e in entries]
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    return {"ok": True, "industries": path, "count": len(data)}


def do_write_rivers(segments: list[RiverSegment], path: str) -> dict[str, Any]:
    segments = list(segments)
    if not segments:
        return {"ok": False, "error": "no segments given"}
    for s in segments:
        if s.kind not in ("river", "sea"):
            return {"ok": False,
                    "error": f"segment {s.name!r}: kind must be river|sea"}
        if len(s.waypoints) < 2:
            return {"ok": False,
                    "error": f"segment {s.name!r}: need >= 2 waypoints"}
    path = _abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = [{"name": s.name, "class": s.kind, "width": s.width,
             "waypoints": [list(wp) for wp in s.waypoints]} for s in segments]
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    return {"ok": True, "rivers": path, "count": len(data)}


def do_write_manifest(heightmap: str, towns: str, industries: str,
                      bounds: Bounds, start_year: int = 2025,
                      save: str = "output.scn",
                      rivers: Optional[str] = None,
                      height: int = 12, rotation: str = "clockwise",
                      seed: int = 20250924, industry_search_radius: int = 32,
                      out: str = "build-manifest.json",
                      timeout: int = 120) -> dict[str, Any]:
    argv = ["--heightmap", _abspath(heightmap), "--towns", _abspath(towns),
            "--industries", _abspath(industries), *bounds.argv(),
            "--start-year", str(start_year), "--height", str(height),
            "--rotation", rotation, "--save", save, "--seed", str(seed),
            "--industry-search-radius", str(industry_search_radius),
            "--out", _abspath(out)]
    if rivers:
        argv += ["--rivers", _abspath(rivers)]
    res = _run("build-manifest.py", argv, timeout)
    if res["ok"]:
        res["manifest"] = _abspath(out)
    return res


def do_validate(heightmap: str, towns: str, industries: str, bounds: Bounds,
                cities: Optional[str] = None, rivers: Optional[str] = None,
                scenario: Optional[str] = None,
                industry_types: Optional[str] = None,
                strict: bool = False,
                timeout: int = 300) -> dict[str, Any]:
    argv = ["--heightmap", _abspath(heightmap), "--towns", _abspath(towns),
            "--industries", _abspath(industries), *bounds.argv()]
    if cities:
        argv += ["--cities", _abspath(cities)]
    if rivers:
        argv += ["--rivers", _abspath(rivers)]
    if scenario:
        argv += ["--scenario", _abspath(scenario)]
    if industry_types:
        argv += ["--industry-types", industry_types]
    if strict:
        argv.append("--strict")
    res = _run("validate-scenario.py", argv, timeout)
    failures = [ln.split("FAIL: ", 1)[1] for ln in
                res.get("output", "").splitlines() if "FAIL:" in ln]
    res["failures"] = failures
    return res


def do_run_builder(manifest: str, engine: str, config: str,
                   timeout: int = 1800) -> dict[str, Any]:
    """Run HSB_BUILD=<manifest> <engine> -e -c <config>.

    Requires a patched + built engine (docs/LOCAL-BUILD.md). The config
    MUST live inside the personal dir (engine derives it from -c).
    Needs a display (Xvfb) + SDL_AUDIODRIVER=dummy — pass through env.
    """
    manifest_p, engine_p, config_p = (_abspath(manifest), _abspath(engine),
                                      _abspath(config))
    for label, p in (("manifest", manifest_p), ("engine", engine_p),
                     ("config", config_p)):
        if not os.path.exists(p):
            return {"ok": False, "error": f"{label} missing: {p}"}
    env = dict(os.environ, HSB_BUILD=manifest_p)
    try:
        proc = subprocess.run([engine_p, "-e", "-c", config_p],
                              capture_output=True, text=True,
                              timeout=timeout, cwd=str(ROOT), env=env)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"builder timed out after {timeout}s"}
    out = (proc.stdout + proc.stderr)[-8000:]
    saved = "hsb: saved" in out
    return {"ok": proc.returncode == 0 and saved,
            "returncode": proc.returncode, "saved": saved, "output": out}


# ---------------------------------------------------------------- server

mcp = MCPServer(
    "openTTD-HSB",
    description="Headless scenario builder for OpenTTD: author data files, "
                "build heightmap/towns/manifest, validate, run the builder.",
    instructions=(
        "Workflow: 1) write_cities_csv (+ optional geocode_towns), "
        "2) build_heightmap with the SAME bounds, 3) build_towns, "
        "4) write_industries (+ write_rivers), 5) validate, "
        "6) write_manifest, 7) run_builder. "
        "Bounds must agree across every step. Read hsb://formats and "
        "hsb://workflow first."),
)


@mcp.tool()
def build_heightmap(bounds: Bounds, zoom: int = 8, size: int = 2048,
                    out: str = "heightmap.png",
                    master_out: str = "heightmap-master.tif") -> dict:
    """Build heightmap PNG from elevation tiles for the given bounds."""
    return do_build_heightmap(bounds, zoom, size, out, master_out)


@mcp.tool()
def write_cities_csv(rows: list[CityRow], path: str) -> dict:
    """Author the cities.csv input (name/lat/lon/pop/city/source per town)."""
    return do_write_cities_csv(rows, path)


@mcp.tool()
def geocode_towns(queries: list[GeocodeQuery], suffix: str = "") -> dict:
    """Geocode town names to lat/lon (Nominatim; suffix e.g. ', Deutschland').
    Returns CSV-ready rows for write_cities_csv."""
    return do_geocode(queries, suffix)


@mcp.tool()
def build_towns(cities: str, heightmap: str, bounds: Bounds,
                out: str = "towns.json",
                debug_out: str = "towns.debug.csv",
                pop_divisor: float = 450.0, pop_min: int = 300,
                sample_only: bool = False) -> dict:
    """Convert cities.csv to OpenTTD town-import towns.json (same bounds!)."""
    return do_build_towns(cities, heightmap, bounds, out, debug_out,
                          pop_divisor, pop_min, sample_only=sample_only)


@mcp.tool()
def write_industries(entries: list[IndustryPlacement], path: str) -> dict:
    """Author industries.json placements (type/lat/lon/near per industry)."""
    return do_write_industries(entries, path)


@mcp.tool()
def write_rivers(segments: list[RiverSegment], path: str) -> dict:
    """Author rivers.json waterways (name/class/width/waypoints)."""
    return do_write_rivers(segments, path)


@mcp.tool()
def validate(heightmap: str, towns: str, industries: str, bounds: Bounds,
             cities: Optional[str] = None, rivers: Optional[str] = None,
             industry_types: Optional[str] = None) -> dict:
    """Validate scenario inputs (schema, bounds, land/water). Fix failures,
    don't proceed to run_builder until ok."""
    return do_validate(heightmap, towns, industries, bounds, cities, rivers,
                       industry_types=industry_types)


@mcp.tool()
def write_manifest(heightmap: str, towns: str, industries: str, bounds: Bounds,
                   start_year: int = 2025, save: str = "output.scn",
                   rivers: Optional[str] = None,
                   seed: int = 20250924, out: str = "build-manifest.json"
                   ) -> dict:
    """Write the HSB_BUILD manifest embedding bounds + absolute paths."""
    return do_write_manifest(heightmap, towns, industries, bounds, start_year,
                             save, rivers, out=out)


@mcp.tool()
def run_builder(manifest: str, engine: str, config: str,
                timeout: int = 1800) -> dict:
    """Run the headless builder (patched engine + config in personal dir).
    Slow (~100 s for 4096); needs display/Xvfb + SDL_AUDIODRIVER=dummy."""
    return do_run_builder(manifest, engine, config, timeout)


@mcp.resource("hsb://formats")
def formats_resource() -> str:
    """Manifest + towns/industries/rivers file schemas."""
    return (ROOT / "docs" / "FORMATS.md").read_text(encoding="utf-8")


@mcp.resource("hsb://workflow")
def workflow_resource() -> str:
    """Step-by-step scenario build runbook."""
    return (ROOT / "docs" / "SCENARIO.md").read_text(encoding="utf-8")


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
