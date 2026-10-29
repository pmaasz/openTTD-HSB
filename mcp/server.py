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
import math
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Annotated, Any, Literal, Optional

from pydantic import AfterValidator, BaseModel, Field, field_validator, model_validator

try:
    from mcp.server.mcpserver import MCPServer
except ImportError:  # pragma: no cover - import-time hint for missing dep
    print("mcp/server.py needs the 'mcp' package (mcp/requirements.txt)",
          file=sys.stderr)
    raise

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"
WORKDIR = Path(os.environ.get("HSB_WORKDIR") or os.getcwd()).expanduser().resolve()
HSB_ENGINE = os.environ.get("HSB_ENGINE")

Latitude = Annotated[float, Field(ge=-90, le=90, allow_inf_nan=False)]
Longitude = Annotated[float, Field(ge=-180, le=180, allow_inf_nan=False)]
Population = Annotated[int, Field(ge=1, le=2_147_483_647)]
PathArg = Annotated[str, Field(min_length=1, max_length=4096)]
Timeout = Annotated[int, Field(ge=1, le=86_400)]
Zoom = Annotated[int, Field(ge=1, le=22)]


def _power_of_two(value: int) -> int:
    if value & (value - 1) != 0:
        raise ValueError("heightmap size must be a power of two")
    return value


HeightmapSize = Annotated[int, Field(ge=64, le=4096), AfterValidator(_power_of_two)]
StartYear = Annotated[int, Field(ge=0, le=5_000_000)]
Seed = Annotated[int, Field(ge=0, le=4_294_967_294)]
IndustrySearchRadius = Annotated[int, Field(ge=1, le=512)]
Rotation = Literal["clockwise", "counter-clockwise"]
IndustryTypes = Annotated[str, Field(max_length=10_000)]


class Bounds(BaseModel):
    """Geographic bounds shared by heightmap, towns, industries, rivers."""

    lat_min: Latitude = Field(description="Southern edge, degrees north")
    lat_max: Latitude = Field(description="Northern edge, degrees north")
    lon_min: Longitude = Field(description="Western edge, degrees east")
    lon_max: Longitude = Field(description="Eastern edge, degrees east")

    @model_validator(mode="after")
    def validate_order(self) -> Bounds:
        self.check()
        return self

    def check(self) -> None:
        if not (self.lat_min < self.lat_max and self.lon_min < self.lon_max):
            raise ValueError(
                "bounds must satisfy lat_min < lat_max and lon_min < lon_max, "
                f"got {self.model_dump()}")

    def argv(self) -> list[str]:
        return ["--lat-min", str(self.lat_min), "--lat-max", str(self.lat_max),
                "--lon-min", str(self.lon_min), "--lon-max", str(self.lon_max)]


class CityRow(BaseModel):
    """One row of the cities.csv input for build-towns."""

    name: str = Field(min_length=1, max_length=200)
    lat: Latitude
    lon: Longitude
    pop_real: Population = Field(description="Real inhabitants (pre-scaling)")
    city: bool = Field(default=False,
                       description="True for OpenTTD cities (large metros)")
    source: str = Field(default="estimate", max_length=500,
                        description="Provenance label, e.g. destatis2023")


class GeocodeQuery(BaseModel):
    """One town name to geocode via Nominatim."""

    name: str = Field(min_length=1, max_length=200)
    pop: Population = Field(description="Real inhabitants (pre-scaling)")

    @field_validator("name")
    @classmethod
    def validate_name(cls, value: str) -> str:
        if any(char in value for char in ("\r", "\n", ",")):
            raise ValueError("geocode names cannot contain commas or newlines")
        return value


class IndustryPlacement(BaseModel):
    """One entry of industries.json."""

    type: str = Field(min_length=1, max_length=200,
                      description="In-game industry display name, "
                                  "e.g. 'Farm', 'Power Plant' (NewGRF-dependent)")
    lat: Latitude
    lon: Longitude
    near: str = Field(default="", max_length=500,
                      description="Free-form label for logs, e.g. town name")


class RiverSegment(BaseModel):
    """One entry of rivers.json."""

    name: str = Field(min_length=1, max_length=200)
    kind: Literal["river", "sea"] = Field(
        default="river",
        description="'river' (inland) or 'sea' (tidal, ports work)")
    width: int = Field(default=1, ge=1, le=3,
                       description="Brush width in tiles at 4096")
    waypoints: Annotated[list[tuple[Latitude, Longitude]], Field(min_length=2)] = Field(
        description="Polyline as [[lat, lon], ...]")


CityRows = Annotated[list[CityRow], Field(min_length=1, max_length=10_000)]
GeocodeQueries = Annotated[list[GeocodeQuery], Field(min_length=1, max_length=1_000)]
IndustryEntries = Annotated[list[IndustryPlacement], Field(min_length=1, max_length=100_000)]
RiverSegments = Annotated[list[RiverSegment], Field(min_length=1, max_length=10_000)]


def _workdir() -> Path:
    if not WORKDIR.is_dir():
        raise ValueError(f"HSB_WORKDIR is not a directory: {WORKDIR}")
    return WORKDIR


def _confined_path(path: str, label: str = "path") -> Path:
    if not isinstance(path, (str, os.PathLike)):
        raise ValueError(f"{label} must be a path string")
    raw = os.fspath(path)
    if not isinstance(raw, str) or not raw.strip() or "\x00" in raw:
        raise ValueError(f"bad {label}: {raw!r}")
    root = _workdir()
    requested = Path(raw).expanduser()
    candidate = requested if requested.is_absolute() else root / requested
    try:
        resolved = candidate.resolve(strict=False)
        resolved.relative_to(root)
    except (OSError, RuntimeError, ValueError) as exc:
        raise ValueError(
            f"{label} must resolve beneath HSB_WORKDIR ({root}): {raw!r}") from exc
    return resolved


def _regular_file(path: Path, label: str) -> Path:
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        raise ValueError(f"{label} missing: {path}") from exc
    if not stat.S_ISREG(mode):
        raise ValueError(f"{label} is not a regular file: {path}")
    return path


def _filename(value: str, label: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError(f"bad {label}: {value!r}")
    if (value in (".", "..") or "/" in value or "\\" in value or ":" in value
            or Path(value).name != value):
        raise ValueError(f"{label} must be a filename, not a path: {value!r}")
    return value


def _configured_engine() -> Path:
    if not HSB_ENGINE:
        raise ValueError("HSB_ENGINE is not configured")
    path = Path(HSB_ENGINE).expanduser()
    if not path.is_absolute():
        path = WORKDIR / path
    try:
        path = path.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ValueError(f"HSB_ENGINE missing: {path}") from exc
    _regular_file(path, "HSB_ENGINE")
    if not os.access(path, os.X_OK):
        raise ValueError(f"HSB_ENGINE is not executable: {path}")
    return path


def _run(script: str, argv: list[str], timeout: int) -> dict[str, Any]:
    """Run a scripts/ helper, capture output. Never raises on script failure."""
    path = SCRIPTS / script
    if not path.is_file():
        return {"ok": False, "error": f"script missing: {path}"}
    try:
        proc = subprocess.run(
            [sys.executable, str(path), *argv],
            capture_output=True, text=True, timeout=timeout,
            cwd=str(_workdir()),
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"{script} timed out after {timeout}s"}
    except OSError as exc:
        return {"ok": False, "error": f"could not run {script}: {exc}"}
    return {
        "ok": proc.returncode == 0,
        "returncode": proc.returncode,
        "output": (proc.stdout + proc.stderr)[-8000:],
    }


def _stage_sources(sources: list[tuple[str, Path]],
                   directory: Path) -> list[Path]:
    root = _workdir()
    try:
        destination_dir = directory.resolve(strict=False)
        destination_dir.relative_to(root)
    except (OSError, RuntimeError, ValueError) as exc:
        raise ValueError(
            f"staging directory escapes HSB_WORKDIR ({root}): {directory}") from exc
    destination_dir.mkdir(parents=True, exist_ok=True)
    destination_dir = destination_dir.resolve(strict=True)
    try:
        destination_dir.relative_to(root)
    except ValueError as exc:
        raise ValueError(
            f"staging directory escapes HSB_WORKDIR ({root}): {directory}") from exc

    temporary: list[Path] = []
    staged: list[tuple[Path, Path]] = []
    try:
        for name, source in sources:
            fd, temporary_name = tempfile.mkstemp(prefix=".hsb-stage-",
                                                   dir=destination_dir)
            temporary_path = Path(temporary_name)
            temporary.append(temporary_path)
            with os.fdopen(fd, "wb") as output:
                with source.open("rb") as input_file:
                    shutil.copyfileobj(input_file, output)
                output.flush()
                os.fsync(output.fileno())
            destination = destination_dir / name
            _confined_path(str(destination), "staging destination")
            staged.append((temporary_path, destination))
        for temporary_path, destination in staged:
            os.replace(temporary_path, destination)
    finally:
        for temporary_path in temporary:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass
    return [destination for _, destination in staged]


def do_build_heightmap(bounds: Bounds, zoom: Zoom = 8,
                       size: HeightmapSize = 2048,
                       sea_level: float = 0.0,
                       out: PathArg = "heightmap.png",
                       master_out: PathArg = "heightmap-master.tif",
                       timeout: Timeout = 1200) -> dict[str, Any]:
    if not math.isfinite(sea_level):
        return {"ok": False, "error": "sea_level must be finite"}
    out_path = _confined_path(out, "heightmap output")
    master_path = _confined_path(master_out, "heightmap master output")
    res = _run("build-heightmap.py", [
        *bounds.argv(), "--zoom", str(zoom), "--size", str(size),
        "--sea-level", str(sea_level),
        "--out", str(out_path), "--master-out", str(master_path),
    ], timeout)
    if res["ok"]:
        res.update({"heightmap": str(out_path), "master": str(master_path)})
    return res


def do_write_cities_csv(rows: CityRows, path: PathArg) -> dict[str, Any]:
    rows = list(rows)
    if not rows:
        return {"ok": False, "error": "no rows given"}
    path = _confined_path(path, "cities path")
    os.makedirs(path.parent, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["name", "lat", "lon", "pop_real", "city", "source"])
        for row in rows:
            writer.writerow([row.name, row.lat, row.lon, row.pop_real,
                             "1" if row.city else "0", row.source])
    return {"ok": True, "cities": str(path), "rows": len(rows)}


def do_geocode(queries: GeocodeQueries, suffix: str = "",
               timeout: Timeout = 600) -> dict[str, Any]:
    """Geocode town names; returns CSV-ready rows (Nominatim, ~1s per name)."""
    queries = list(queries)
    if not queries:
        return {"ok": False, "error": "no queries given"}
    with tempfile.TemporaryDirectory(prefix=".hsb-geocode-",
                                     dir=_workdir()) as temporary:
        names_path = Path(temporary) / "queries.txt"
        with names_path.open("w", encoding="utf-8") as output:
            for query in queries:
                output.write(f"{query.name},{query.pop}\n")
        argv = ["--geocode", str(names_path), "--cities", "x",
                "--heightmap", "x", "--lat-min", "0", "--lat-max", "1",
                "--lon-min", "0", "--lon-max", "1"]
        if suffix:
            argv += ["--geocode-suffix", suffix]
        res = _run("build-towns.py", argv, timeout)

    subprocess_ok = bool(res.get("ok"))
    rows: list[dict[str, Any]] = []
    failed: list[str] = []
    for line in res.get("output", "").splitlines():
        line = line.strip()
        if line.startswith("# FAILED"):
            failed.append(line)
        elif line and not line.startswith("["):
            try:
                name, lat, lon, pop, city, source = line.split(",")
                lat_value, lon_value = float(lat), float(lon)
                if (not math.isfinite(lat_value) or not math.isfinite(lon_value)
                        or not -90.0 <= lat_value <= 90.0
                        or not -180.0 <= lon_value <= 180.0):
                    raise ValueError("geocoder returned invalid coordinates")
                rows.append({"name": name, "lat": lat_value,
                             "lon": lon_value, "pop_real": int(pop),
                             "city": city == "1", "source": source})
            except ValueError:
                pass
    res.update({
        "rows": rows,
        "failed": failed,
        "expected_rows": len(queries),
        "ok": subprocess_ok and not failed and len(rows) == len(queries),
    })
    return res


def do_build_towns(cities: PathArg, heightmap: PathArg, bounds: Bounds,
                   out: PathArg = "towns.json",
                   debug_out: PathArg = "towns.debug.csv",
                   pop_divisor: Annotated[float, Field(gt=0, le=1_000_000,
                                                      allow_inf_nan=False)] = 450.0,
                   pop_min: Population = 300,
                   nudge_px: Annotated[int, Field(ge=0, le=1024)] = 4,
                   sample_only: bool = False, strict: bool = False,
                   timeout: Timeout = 600) -> dict[str, Any]:
    cities_path = _confined_path(cities, "cities path")
    heightmap_path = _confined_path(heightmap, "heightmap path")
    out_path = _confined_path(out, "towns output")
    debug_path = _confined_path(debug_out, "towns debug output")
    argv = ["--cities", str(cities_path), "--heightmap", str(heightmap_path),
            *bounds.argv(), "--out", str(out_path),
            "--debug-out", str(debug_path),
            "--pop-divisor", str(pop_divisor), "--pop-min", str(pop_min),
            "--nudge-px", str(nudge_px)]
    if sample_only:
        argv.append("--sample")
    if strict:
        argv.append("--strict")
    res = _run("build-towns.py", argv, timeout)
    if res["ok"]:
        try:
            with out_path.open(encoding="utf-8") as input_file:
                towns = json.load(input_file)
            if not isinstance(towns, list):
                raise TypeError("towns output must contain a JSON array")
            res.update({"towns": str(out_path), "debug": str(debug_path),
                        "count": len(towns),
                        "cities": sum(1 for town in towns if town.get("city"))})
        except (OSError, json.JSONDecodeError, TypeError) as exc:
            res.update({"ok": False, "error": str(exc)})
    return res


def do_write_industries(entries: IndustryEntries,
                        path: PathArg) -> dict[str, Any]:
    entries = list(entries)
    if not entries:
        return {"ok": False, "error": "no entries given"}
    path = _confined_path(path, "industries path")
    os.makedirs(path.parent, exist_ok=True)
    data = [{"type": entry.type, "lat": entry.lat, "lon": entry.lon,
              "near": entry.near or entry.type} for entry in entries]
    with path.open("w", encoding="utf-8") as output:
        json.dump(data, output, ensure_ascii=False, indent=2)
    return {"ok": True, "industries": str(path), "count": len(data)}


def do_write_rivers(segments: RiverSegments, path: PathArg) -> dict[str, Any]:
    segments = list(segments)
    if not segments:
        return {"ok": False, "error": "no segments given"}
    for segment in segments:
        if segment.kind not in ("river", "sea"):
            return {"ok": False,
                    "error": f"segment {segment.name!r}: kind must be river|sea"}
        if len(segment.waypoints) < 2:
            return {"ok": False,
                    "error": f"segment {segment.name!r}: need >= 2 waypoints"}
    path = _confined_path(path, "rivers path")
    os.makedirs(path.parent, exist_ok=True)
    data = [{"name": segment.name, "class": segment.kind, "width": segment.width,
             "waypoints": [list(waypoint) for waypoint in segment.waypoints]}
            for segment in segments]
    with path.open("w", encoding="utf-8") as output:
        json.dump(data, output, ensure_ascii=False, indent=2)
    return {"ok": True, "rivers": str(path), "count": len(data)}


def do_write_manifest(heightmap: PathArg, towns: PathArg,
                      industries: PathArg, bounds: Bounds,
                      start_year: StartYear = 2025,
                      save: str = "output.scn",
                      rivers: Optional[PathArg] = None,
                      height: Annotated[int, Field(ge=1, le=255)] = 12,
                      snow_line: Annotated[int, Field(ge=0, le=255)] = 0,
                      rotation: Rotation = "clockwise", seed: Seed = 20250924,
                      industry_search_radius: IndustrySearchRadius = 32,
                      max_loan: Annotated[int, Field(ge=0, le=4_000_000_000)] = 0,
                      competitors: Annotated[int, Field(ge=0, le=15)] = 0,
                      currency: str = "",
                      roads: Optional[PathArg] = None,
                      autobahnen: Optional[PathArg] = None,
                      rail: Optional[PathArg] = None,
                      airports: Optional[PathArg] = None,
                      harbours: Optional[PathArg] = None,
                      stations: Optional[PathArg] = None,
                      osm: Optional[PathArg] = None,
                      with_rail: bool = True,
                      with_fleet: bool = True,
                      network_money: Annotated[int, Field(ge=0, le=4_000_000_000)] = 0,
                      bus_towns: Optional[Annotated[int, Field(ge=0, le=1024)]] = None,
                      trains: Optional[Annotated[int, Field(ge=0, le=1024)]] = None,
                      out: PathArg = "build-manifest.json",
                      timeout: Timeout = 120) -> dict[str, Any]:
    heightmap_path = _confined_path(heightmap, "heightmap path")
    towns_path = _confined_path(towns, "towns path")
    industries_path = _confined_path(industries, "industries path")
    out_path = _confined_path(out, "manifest output")
    save_name = _filename(save, "save")
    argv = ["--heightmap", str(heightmap_path), "--towns", str(towns_path),
            "--industries", str(industries_path), *bounds.argv(),
            "--start-year", str(start_year), "--height", str(height),
            "--snow-line", str(snow_line),
            "--rotation", rotation, "--save", save_name, "--seed", str(seed),
            "--industry-search-radius", str(industry_search_radius),
            "--max-loan", str(max_loan), "--competitors", str(competitors),
            "--out", str(out_path)]
    if currency:
        argv += ["--currency", currency]
    if rivers:
        rivers_path = _confined_path(rivers, "rivers path")
        argv += ["--rivers", str(rivers_path)]
    network = False
    for label, path in (("roads", roads), ("autobahnen", autobahnen),
                        ("rail", rail), ("airports", airports),
                        ("harbours", harbours), ("stations", stations)):
        if not path:
            continue
        network = True
        argv += [f"--{label}", str(_confined_path(path, f"{label} path"))]
    if osm:
        argv += ["--osm", str(_confined_path(osm, "osm directory"))]
    if network:
        # The infrastructure company that owns the network needs a company slot of
        # its own, and the player needs a second one; write_manifest is where an
        # agent finds that out rather than an hour into a build.
        if competitors < 2:
            return {"ok": False, "output": (
                "competitors must be at least 2 when a network is built: the "
                "infrastructure company takes one slot and the player needs "
                "another. No manifest was written.")}
        argv += ["--network-money", str(network_money)]
        if bus_towns is not None:
            argv += ["--bus-towns", str(bus_towns)]
        if trains is not None:
            argv += ["--trains", str(trains)]
        if not with_rail:
            argv.append("--no-rail")
        if not with_fleet:
            argv.append("--no-fleet")
    res = _run("build-manifest.py", argv, timeout)
    if res["ok"]:
        res["manifest"] = str(out_path)
    return res


def do_validate(heightmap: PathArg, towns: PathArg, industries: PathArg,
                bounds: Bounds, cities: Optional[PathArg] = None,
                rivers: Optional[PathArg] = None,
                roads: Optional[PathArg] = None,
                autobahnen: Optional[PathArg] = None,
                rail: Optional[PathArg] = None,
                airports: Optional[PathArg] = None,
                harbours: Optional[PathArg] = None,
                stations: Optional[PathArg] = None,
                osm: Optional[PathArg] = None,
                scenario: Optional[PathArg] = None,
                industry_types: Optional[IndustryTypes] = None, strict: bool = False,
                timeout: Timeout = 300) -> dict[str, Any]:
    heightmap_path = _confined_path(heightmap, "heightmap path")
    towns_path = _confined_path(towns, "towns path")
    industries_path = _confined_path(industries, "industries path")
    argv = ["--heightmap", str(heightmap_path), "--towns", str(towns_path),
            "--industries", str(industries_path), *bounds.argv()]
    if cities:
        argv += ["--cities", str(_confined_path(cities, "cities path"))]
    if rivers:
        argv += ["--rivers", str(_confined_path(rivers, "rivers path"))]
    for label, path in (("roads", roads), ("autobahnen", autobahnen),
                        ("rail", rail), ("airports", airports),
                        ("harbours", harbours), ("stations", stations)):
        if path:
            argv += [f"--{label}", str(_confined_path(path, f"{label} path"))]
    if osm:
        argv += ["--osm", str(_confined_path(osm, "osm directory"))]
    if scenario:
        argv += ["--scenario", str(_confined_path(scenario, "scenario path"))]
    if industry_types:
        argv += ["--industry-types", industry_types]
    if strict:
        argv.append("--strict")
    res = _run("validate-scenario.py", argv, timeout)
    failures = [line.split("FAIL: ", 1)[1] for line in
                res.get("output", "").splitlines() if "FAIL:" in line]
    res["failures"] = failures
    return res


def do_run_builder(manifest: PathArg, config: PathArg,
                   timeout: Timeout = 1800) -> dict[str, Any]:
    try:
        engine_path = _configured_engine()
        manifest_path = _regular_file(
            _confined_path(manifest, "manifest path"), "manifest")
        config_path = _regular_file(
            _confined_path(config, "config path"), "config")
        with manifest_path.open(encoding="utf-8") as input_file:
            manifest_data = json.load(input_file)
        if not isinstance(manifest_data, dict):
            raise ValueError("manifest must contain a JSON object")

        source_specs: list[tuple[str, str, Path]] = []
        for key in ("heightmap", "towns", "industries"):
            raw_path = manifest_data.get(key)
            if not isinstance(raw_path, str) or not raw_path:
                raise ValueError(f"manifest {key} must be a path string")
            if not Path(raw_path).is_absolute():
                raise ValueError(f"manifest {key} must be an absolute path")
            source_path = _confined_path(raw_path, f"manifest {key}")
            _regular_file(source_path, f"manifest {key}")
            source_name = _filename(Path(raw_path).name, f"manifest {key} basename")
            source_specs.append((key, source_name, source_path))

        for key in ("rivers",):
            raw_path = manifest_data.get(key)
            if raw_path is None or raw_path == "":
                continue
            if not isinstance(raw_path, str):
                raise ValueError(f"manifest {key} must be a path string or null")
            if not Path(raw_path).is_absolute():
                raise ValueError(f"manifest {key} must be an absolute path")
            source_path = _confined_path(raw_path, f"manifest {key}")
            _regular_file(source_path, f"manifest {key}")
            source_specs.append((key, "", source_path))

        _filename(manifest_data.get("save"), "manifest save")
        staging_specs = [(name, source) for key, name, source in source_specs
                         if key in ("heightmap", "towns")]
        if len({name for name, _ in staging_specs}) != len(staging_specs):
            raise ValueError("manifest heightmap and towns basenames must differ")
        staged = _stage_sources(
            staging_specs,
            config_path.parent / "scenario" / "heightmap",
        )
    except (OSError, RuntimeError, TypeError, ValueError) as exc:
        return {"ok": False, "error": str(exc)}

    env = dict(os.environ, HSB_BUILD=str(manifest_path))
    try:
        proc = subprocess.run(
            [str(engine_path), "-e", "-c", str(config_path)],
            capture_output=True, text=True, timeout=timeout,
            cwd=str(_workdir()), env=env,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"builder timed out after {timeout}s"}
    except OSError as exc:
        return {"ok": False, "error": f"could not run builder: {exc}"}
    output = (proc.stdout + proc.stderr)[-8000:]
    saved = "hsb: saved" in output
    return {"ok": proc.returncode == 0 and saved,
            "returncode": proc.returncode, "saved": saved,
            "audit": _parse_audit(output),
            "staged": [str(path) for path in staged], "output": output}


def _parse_kv_line(output: str, marker: str) -> dict[str, Any]:
    """Pull one `marker k=v ...` line out of the engine log into a dict."""
    for line in output.splitlines():
        _, found, payload = line.partition(marker)
        if not found:
            continue
        parsed: dict[str, Any] = {}
        for field in payload.split():
            key, separator, value = field.partition("=")
            if not separator:
                continue
            try:
                parsed[key] = int(value)
            except ValueError:
                try:
                    parsed[key] = float(value)
                except ValueError:
                    parsed[key] = value
        if parsed:
            return parsed
    return {}


def _parse_audit(output: str) -> dict[str, Any]:
    """Pull the builder's measurement lines out of the engine log.

    The builder measures what it produced (towns on water, how much carved
    river a ship can reach, and when the network phase ran, what it connected).
    Returning the numbers as a dict means an agent can assert on them instead of
    reading a log line: towns_on_water must be 0, and carved_reachable should
    equal carved.

    Two markers, not one, because the two measurements are independent: a
    manifest that names no network file produces no network line at all, and a
    gate that required every key to be present would fail a terrain-only build.
    They are merged into one dict, with the network keys a plain name can never
    collide with.
    """
    audit = _parse_kv_line(output, "hsb: audit: ")
    audit.update({f"network_{key}": value
                  for key, value in _parse_kv_line(output, "hsb: network audit: ").items()})
    return audit


def do_render_preview(heightmap: PathArg, towns: PathArg, industries: PathArg,
                      bounds: Bounds, out: PathArg = "preview.png",
                      rivers: Optional[PathArg] = None,
                      size: HeightmapSize = 1024, palette: bool = False,
                      allow_town_on_water: bool = False,
                      timeout: Timeout = 300) -> dict[str, Any]:
    """Render terrain + rivers + towns + industries, and fail on towns in water."""
    out_path = _confined_path(out, "preview output")
    argv = ["--heightmap", str(_confined_path(heightmap, "heightmap path")),
            "--towns", str(_confined_path(towns, "towns path")),
            "--industries", str(_confined_path(industries, "industries path")),
            *bounds.argv(), "--size", str(size), "--out", str(out_path)]
    if rivers:
        argv += ["--rivers", str(_confined_path(rivers, "rivers path"))]
    if palette:
        argv.append("--palette")
    if allow_town_on_water:
        argv.append("--allow-town-on-water")
    res = _run("render-preview.py", argv, timeout)
    # The script logs "[render-preview] ON WATER (N): name, name".
    on_water: list[str] = []
    for line in res.get("output", "").splitlines():
        _, marker, payload = line.partition("ON WATER (")
        if not marker or "):" not in payload:
            continue
        on_water.extend(name.strip() for name in
                        payload.split("):", 1)[1].split(",") if name.strip())
    res["towns_on_water"] = on_water
    if res["ok"]:
        res["preview"] = str(out_path)
    return res


mcp = MCPServer(
    "openTTD-HSB",
    description="Headless scenario builder for OpenTTD: author data files, "
                "build heightmap/towns/manifest, validate, run the builder.",
    instructions=(
        "Workflow: 1) write_cities_csv (+ optional geocode_towns), "
        "2) build_heightmap with the SAME bounds, 3) build_towns, "
        "4) write_industries (+ write_rivers), 5) render_preview, "
        "6) validate, 7) write_manifest, 8) run_builder. "
        "Bounds must agree across every step. render_preview fails when a town "
        "anchor lands on water, which is cheaper to fix than a rebuild. "
        "run_builder returns an `audit` dict; require towns_on_water == 0 and "
        "carved_reachable == carved. Read hsb://formats and hsb://workflow first."),
)


@mcp.tool()
def build_heightmap(bounds: Bounds, zoom: Zoom = 8,
                    size: HeightmapSize = 2048,
                    sea_level: float = 0.0,
                    out: PathArg = "heightmap.png",
                    master_out: PathArg = "heightmap-master.tif",
                    timeout: Timeout = 1200) -> dict[str, Any]:
    """Build heightmap PNG from elevation tiles for the given bounds.

    sea_level is the elevation in metres below which a pixel counts as water;
    pass a small negative value (e.g. -2) to drop SRTM tidal flats and
    reclaimed land that would otherwise render as unbuildable ground.
    """
    return do_build_heightmap(bounds, zoom, size, sea_level, out, master_out, timeout)


@mcp.tool()
def write_cities_csv(rows: CityRows, path: PathArg) -> dict[str, Any]:
    """Author the cities.csv input (name/lat/lon/pop/city/source per town)."""
    return do_write_cities_csv(rows, path)


@mcp.tool()
def geocode_towns(queries: GeocodeQueries,
                  suffix: Annotated[str, Field(max_length=200)] = "",
                  timeout: Timeout = 600) -> dict[str, Any]:
    """Geocode town names to lat/lon (Nominatim; suffix e.g. ', Deutschland').
    Returns CSV-ready rows for write_cities_csv."""
    return do_geocode(queries, suffix, timeout)


@mcp.tool()
def build_towns(cities: PathArg, heightmap: PathArg, bounds: Bounds,
                out: PathArg = "towns.json",
                debug_out: PathArg = "towns.debug.csv",
                pop_divisor: Annotated[float, Field(gt=0, le=1_000_000,
                                                   allow_inf_nan=False)] = 450.0,
                pop_min: Population = 300,
                nudge_px: Annotated[int, Field(ge=0, le=1024)] = 4,
                sample_only: bool = False, strict: bool = False,
                timeout: Timeout = 600) -> dict[str, Any]:
    """Convert cities.csv to OpenTTD town-import towns.json (same bounds!)."""
    return do_build_towns(cities, heightmap, bounds, out, debug_out,
                          pop_divisor, pop_min, nudge_px, sample_only,
                          strict, timeout)


@mcp.tool()
def write_industries(entries: IndustryEntries,
                     path: PathArg) -> dict[str, Any]:
    """Author industries.json placements (type/lat/lon/near per industry)."""
    return do_write_industries(entries, path)


@mcp.tool()
def write_rivers(segments: RiverSegments, path: PathArg) -> dict[str, Any]:
    """Author rivers.json waterways (name/class/width/waypoints)."""
    return do_write_rivers(segments, path)


@mcp.tool()
def validate(heightmap: PathArg, towns: PathArg, industries: PathArg,
             bounds: Bounds, cities: Optional[PathArg] = None,
             rivers: Optional[PathArg] = None,
             roads: Optional[PathArg] = None,
             autobahnen: Optional[PathArg] = None,
             rail: Optional[PathArg] = None,
             airports: Optional[PathArg] = None,
             harbours: Optional[PathArg] = None,
             stations: Optional[PathArg] = None,
             osm: Optional[PathArg] = None,
             scenario: Optional[PathArg] = None,
             industry_types: Optional[IndustryTypes] = None,
             strict: bool = False, timeout: Timeout = 300) -> dict[str, Any]:
    """Validate scenario inputs (schema, bounds, land/water). Fix failures,
    don't proceed to run_builder until ok. The network inputs are validated the
    same way: corridors as {name, waypoints}, airports and harbours as {name, xy}."""
    return do_validate(heightmap, towns, industries, bounds, cities, rivers,
                       roads, autobahnen, rail, airports, harbours, stations,
                       osm, scenario, industry_types, strict, timeout)


@mcp.tool()
def render_preview(heightmap: PathArg, towns: PathArg, industries: PathArg,
                   bounds: Bounds, out: PathArg = "preview.png",
                   rivers: Optional[PathArg] = None,
                   size: HeightmapSize = 1024, palette: bool = False,
                   allow_town_on_water: bool = False,
                   timeout: Timeout = 300) -> dict[str, Any]:
    """Render terrain + rivers + towns + industries as a PNG, and FAIL when a
    town anchor lands on water. Cheaper than a full rebuild; run it before
    write_manifest. Set allow_town_on_water to report collisions without failing.
    """
    return do_render_preview(heightmap, towns, industries, bounds, out, rivers,
                             size, palette, allow_town_on_water, timeout)


@mcp.tool()
def write_manifest(heightmap: PathArg, towns: PathArg, industries: PathArg,
                   bounds: Bounds, start_year: StartYear = 2025,
                   save: str = "output.scn", rivers: Optional[PathArg] = None,
                   height: Annotated[int, Field(ge=1, le=255)] = 12,
                   snow_line: Annotated[int, Field(ge=0, le=255)] = 0,
                   rotation: Rotation = "clockwise", seed: Seed = 20250924,
                   industry_search_radius: IndustrySearchRadius = 32,
                   max_loan: Annotated[int, Field(ge=0, le=4_000_000_000)] = 0,
                   competitors: Annotated[int, Field(ge=0, le=15)] = 0,
                   currency: str = "",
                   roads: Optional[PathArg] = None,
                   autobahnen: Optional[PathArg] = None,
                   rail: Optional[PathArg] = None,
                   airports: Optional[PathArg] = None,
                   harbours: Optional[PathArg] = None,
                   stations: Optional[PathArg] = None,
                   osm: Optional[PathArg] = None,
                   with_rail: bool = True,
                   with_fleet: bool = True,
                   network_money: Annotated[int, Field(ge=0, le=4_000_000_000)] = 0,
                   bus_towns: Optional[Annotated[int, Field(ge=0, le=1024)]] = None,
                   trains: Optional[Annotated[int, Field(ge=0, le=1024)]] = None,
                   out: PathArg = "build-manifest.json",
                   timeout: Timeout = 120) -> dict[str, Any]:
    """Write the HSB_BUILD manifest embedding bounds + absolute paths.

    competitors defaults to 0: a scenario with no company slots drops the player
    onto the map as a spectator. currency (e.g. "EUR") is pinned in the manifest
    rather than inherited from the -c config, which is machine state.

    Naming any network input turns the network phase on: roads.json and
    autobahn.json are corridors, rail.json is corridors, airports.json and
    harbours.json are {name, xy}. The network needs competitors >= 2 - its own
    company takes one slot and the player needs another - and this refuses before
    writing rather than after an hour of routing. run_builder's audit reports
    network_roads, network_intercity_towns, network_vehicles_parked and the rest.
    """
    return do_write_manifest(heightmap, towns, industries, bounds, start_year,
                             save, rivers, height, snow_line, rotation, seed,
                             industry_search_radius, max_loan, competitors,
                             currency, roads, autobahnen, rail, airports,
                             harbours, stations, osm, with_rail, with_fleet,
                             network_money, bus_towns, trains, out, timeout)


@mcp.tool()
def run_builder(manifest: PathArg, config: PathArg,
                timeout: Timeout = 1800) -> dict[str, Any]:
    """Run the configured HSB_ENGINE; stage heightmap and towns automatically."""
    return do_run_builder(manifest, config, timeout)


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
