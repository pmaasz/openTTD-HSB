#!/usr/bin/env python3
"""Smoke-test the HSB MCP server without engine or network.

Synthetic 64x64 map: data authoring -> towns -> manifest -> validate,
plus MCP tool registration + dispatch. Run: <venv>/bin/python mcp/test_smoke.py
from the repo root. Part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
import asyncio
import json
import os
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import server as hsb

B = hsb.Bounds(lat_min=10.0, lat_max=20.0, lon_min=30.0, lon_max=40.0)


def check(name, res):
    assert res.get("ok"), f"{name} FAILED: {json.dumps(res)[:500]}"
    print(f"[smoke] {name}: OK")


def main():
    tmp = tempfile.mkdtemp(prefix="hsb-mcp-smoke-")
    gray = np.full((64, 64), 100, dtype=np.uint8)
    gray[0:2, :] = 0
    gray[-2:, :] = 0
    gray[:, 0:2] = 0
    gray[:, -2:] = 0
    png = os.path.join(tmp, "map.png")
    Image.fromarray(gray, mode="L").save(png)

    cities = os.path.join(tmp, "cities.csv")
    check("write_cities_csv", hsb.do_write_cities_csv([
        hsb.CityRow(name="Alpha", lat=18.0, lon=32.0, pop_real=90000,
                    city=True, source="test"),
        hsb.CityRow(name="Beta", lat=12.0, lon=38.0, pop_real=4500,
                    source="test"),
    ], cities))

    towns = os.path.join(tmp, "towns.json")
    res = hsb.do_build_towns(cities, png, B, out=towns,
                             debug_out=os.path.join(tmp, "towns.debug.csv"))
    check("build_towns", res)
    assert res["count"] == 2, res

    inds = os.path.join(tmp, "industries.json")
    check("write_industries", hsb.do_write_industries([
        hsb.IndustryPlacement(type="Farm", lat=12.0, lon=38.0, near="Beta"),
    ], inds))

    rivers = os.path.join(tmp, "rivers.json")
    check("write_rivers", hsb.do_write_rivers([
        hsb.RiverSegment(name="Test", kind="river", width=1,
                         waypoints=[(18.0, 32.0), (12.0, 38.0)]),
    ], rivers))
    bad = hsb.do_write_rivers(
        [hsb.RiverSegment(name="Bad", kind="ocean", width=1,
                          waypoints=[(18.0, 32.0), (12.0, 38.0)])], rivers)
    assert not bad["ok"], "bad river kind must be rejected"

    manifest = os.path.join(tmp, "build.json")
    check("write_manifest", hsb.do_write_manifest(
        png, towns, inds, B, save="test.scn", rivers=rivers, out=manifest))
    m = json.load(open(manifest, encoding="utf-8"))
    assert m["bounds"] == {"lat_min": 10.0, "lat_max": 20.0,
                           "lon_min": 30.0, "lon_max": 40.0}, m["bounds"]

    check("validate", hsb.do_validate(png, towns, inds, B, cities=cities,
                                      rivers=rivers))

    async def mcp_check():
        tools = await hsb.mcp.list_tools()
        names = sorted(t.name for t in tools)
        for want in ("build_heightmap", "write_cities_csv", "geocode_towns",
                     "build_towns", "write_industries", "write_rivers",
                     "validate", "write_manifest", "run_builder"):
            assert want in names, f"tool missing: {want} (have {names})"
        print(f"[smoke] list_tools: OK ({len(names)} tools)")
        res = await hsb.mcp.call_tool("validate", {
            "heightmap": png, "towns": towns, "industries": inds,
            "bounds": {"lat_min": 10.0, "lat_max": 20.0,
                       "lon_min": 30.0, "lon_max": 40.0},
            "cities": cities, "rivers": rivers})
        assert not res.is_error, res
        print("[smoke] call_tool(validate): OK")

    asyncio.run(mcp_check())
    print("[smoke] ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
