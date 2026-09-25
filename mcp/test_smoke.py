#!/usr/bin/env python3
"""Smoke-test the HSB MCP server without a network or real engine.

Run: <venv>/bin/python mcp/test_smoke.py from the repository root.
Part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
import asyncio
import importlib.util
import json
import os
import stat
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image
from mcp.server.mcpserver.exceptions import ToolError
from pydantic import TypeAdapter, ValidationError


def check(name, result):
    assert result.get("ok"), f"{name} FAILED: {json.dumps(result)[:500]}"
    print(f"[smoke] {name}: OK")


def expect_value_error(callback, label):
    try:
        callback()
    except ValueError:
        return
    raise AssertionError(f"{label} was accepted")


def write_fake_engine(path, log_path):
    source = (
        "#!/usr/bin/env python3\n"
        "import json, os, sys\n"
        f"open({str(log_path)!r}, 'w', encoding='utf-8').write(json.dumps({{\n"
        "    'argv': sys.argv[1:],\n"
        "    'cwd': os.getcwd(),\n"
        "    'manifest': os.environ.get('HSB_BUILD'),\n"
        "}))\n"
        "print('hsb: saved')\n"
    )
    path.write_text(source, encoding="utf-8")
    path.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR |
               stat.S_IRGRP | stat.S_IXGRP | stat.S_IROTH | stat.S_IXOTH)


def run_smoke(temp_root):
    root = Path(temp_root)
    work = root / "work"
    outside = root / "outside"
    work.mkdir()
    outside.mkdir()
    engine_log = root / "engine.json"
    fake_engine = root / "fake-openttd"
    write_fake_engine(fake_engine, engine_log)

    os.environ["HSB_WORKDIR"] = str(work)
    os.environ["HSB_ENGINE"] = str(fake_engine)
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import server as hsb

    heightmap_path = Path(__file__).resolve().parent.parent / "scripts" / "build-heightmap.py"
    spec = importlib.util.spec_from_file_location("build_heightmap", heightmap_path)
    heightmap_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(heightmap_module)
    z = 9
    y0 = heightmap_module.lat_to_ytile(55.5, z)
    y1 = heightmap_module.lat_to_ytile(47.0, z)
    lat_top = heightmap_module.ytile_to_lat(y0, z)
    lat_bottom = heightmap_module.ytile_to_lat(y1 + 1, z)
    mercator_mid = float(heightmap_module.linear_rows_to_mercator_rows([51.25], z, y0, y1)[0])
    linear_mid = (lat_top - 51.25) / (lat_top - lat_bottom)
    assert abs(mercator_mid - linear_mid) > 0.005, (mercator_mid, linear_mid)
    assert heightmap_module.main([
        "--check-only", "--size", "8192", "--lat-min", "10", "--lat-max", "20",
        "--lon-min", "30", "--lon-max", "40",
    ]) == 2
    print("[smoke] linear heightmap reprojection: OK")

    bounds = hsb.Bounds(lat_min=10.0, lat_max=20.0,
                        lon_min=30.0, lon_max=40.0)

    expect_value_error(
        lambda: hsb.Bounds(lat_min=float("nan"), lat_max=20.0,
                           lon_min=30.0, lon_max=40.0),
        "non-finite bounds",
    )
    expect_value_error(
        lambda: hsb.Bounds(lat_min=10.0, lat_max=10.0,
                           lon_min=30.0, lon_max=40.0),
        "unordered bounds",
    )
    try:
        TypeAdapter(hsb.HeightmapSize).validate_python(65)
    except ValidationError:
        pass
    else:
        raise AssertionError("non-power-of-two heightmap size was accepted")
    expect_value_error(
        lambda: hsb.RiverSegment(name="Bad", kind="ocean", width=1,
                                 waypoints=[(18.0, 32.0), (12.0, 38.0)]),
        "invalid river kind",
    )
    expect_value_error(
        lambda: hsb.RiverSegment(name="Bad", width=1,
                                 waypoints=[(float("inf"), 32.0),
                                            (12.0, 38.0)]),
        "non-finite waypoint",
    )

    outside_file = outside / "cities.csv"
    outside_file.write_text("unchanged", encoding="utf-8")
    outside_link = work / "outside-link"
    outside_link.symlink_to(outside, target_is_directory=True)
    file_link = work / "escape.csv"
    file_link.symlink_to(outside_file)
    for bad_path in ("../outside/cities.csv", str(outside_file),
                     "outside-link/cities.csv", "escape.csv"):
        expect_value_error(
            lambda bad_path=bad_path: hsb.do_write_cities_csv([
                hsb.CityRow(name="Alpha", lat=18.0, lon=32.0,
                            pop_real=90000, source="test"),
            ], bad_path),
            f"confined path {bad_path!r}",
        )
    assert outside_file.read_text(encoding="utf-8") == "unchanged"

    gray = np.full((64, 64), 100, dtype=np.uint8)
    gray[:2, :] = 0
    gray[-2:, :] = 0
    gray[:, :2] = 0
    gray[:, -2:] = 0
    Image.fromarray(gray).save(work / "map.png")

    cities_result = hsb.do_write_cities_csv([
        hsb.CityRow(name="Alpha", lat=18.0, lon=32.0, pop_real=90000,
                    city=True, source="test"),
        hsb.CityRow(name="Beta", lat=12.0, lon=38.0, pop_real=4500,
                    source="test"),
    ], "inputs/cities.csv")
    check("write_cities_csv", cities_result)
    assert Path(cities_result["cities"]).is_relative_to(work)

    real_run = hsb._run
    helper_cwds = []

    def checked_run(script, argv, timeout):
        helper_cwds.append(hsb.WORKDIR)
        return real_run(script, argv, timeout)

    hsb._run = checked_run
    try:
        towns_result = hsb.do_build_towns(
            "inputs/cities.csv", "map.png", bounds,
            out="outputs/towns.json",
            debug_out="outputs/towns.debug.csv", strict=True,
        )
        check("build_towns", towns_result)
        assert towns_result["count"] == 2, towns_result

        industries_result = hsb.do_write_industries([
            hsb.IndustryPlacement(type="Farm", lat=12.0, lon=38.0,
                                  near="Beta"),
        ], "inputs/industries.json")
        check("write_industries", industries_result)

        rivers_result = hsb.do_write_rivers([
            hsb.RiverSegment(name="Test", kind="river", width=1,
                             waypoints=[(18.0, 32.0), (12.0, 38.0)]),
        ], "inputs/rivers.json")
        check("write_rivers", rivers_result)

        manifest_result = hsb.do_write_manifest(
            "map.png", "outputs/towns.json", "inputs/industries.json", bounds,
            save="test.scn", rivers="inputs/rivers.json", height=14,
            rotation="counter-clockwise", seed=424242,
            industry_search_radius=48, out="outputs/build.json",
        )
        check("write_manifest", manifest_result)

        scenario = work / "built.scn"
        scenario.write_bytes(b"x" * 100_001)
        validate_result = hsb.do_validate(
            "map.png", "outputs/towns.json", "inputs/industries.json", bounds,
            cities="inputs/cities.csv", rivers="inputs/rivers.json",
            scenario="built.scn", industry_types="Farm", strict=True,
        )
        check("validate", validate_result)
    finally:
        hsb._run = real_run

    assert helper_cwds and all(path == work for path in helper_cwds)
    manifest_data = json.loads(
        (work / "outputs/build.json").read_text(encoding="utf-8"))
    assert manifest_data["seed"] == 424242, manifest_data
    assert manifest_data["heightmap_height"] == 14, manifest_data
    assert manifest_data["heightmap_rotation"] == "counter-clockwise", manifest_data
    assert manifest_data["industry_search_radius"] == 48, manifest_data

    (work / "bad-towns.json").write_text("{}", encoding="utf-8")
    malformed = hsb.do_validate(
        "map.png", "bad-towns.json", "inputs/industries.json", bounds,
        scenario="built.scn")
    assert not malformed["ok"], malformed
    assert "Traceback" not in malformed.get("output", ""), malformed
    assert any("top-level value must be an array" in failure
               for failure in malformed["failures"]), malformed
    Image.new("RGB", (64, 64), (1, 2, 3)).save(work / "bad-map.png")
    malformed = hsb.do_validate(
        "bad-map.png", "outputs/towns.json", "inputs/industries.json", bounds,
        scenario="built.scn")
    assert not malformed["ok"], malformed
    assert "Traceback" not in malformed.get("output", ""), malformed
    assert any("heightmap" in failure for failure in malformed["failures"]), malformed
    print("[smoke] malformed validator inputs: OK")

    queries = [
        hsb.GeocodeQuery(name="Alpha", pop=90000),
        hsb.GeocodeQuery(name="Beta", pop=4500),
    ]
    all_rows = (
        "Alpha,18.0,32.0,90000,0,estimate\n"
        "Beta,12.0,38.0,4500,0,estimate\n"
    )
    geocode_mode = "all"

    def fake_geocode_run(script, argv, timeout):
        assert script == "build-towns.py"
        assert hsb.WORKDIR == work
        names_path = Path(argv[argv.index("--geocode") + 1])
        assert names_path.is_relative_to(work)
        assert len(names_path.read_text(encoding="utf-8").splitlines()) == 2
        if geocode_mode == "missing":
            output = "Alpha,18.0,32.0,90000,0,estimate\n"
        elif geocode_mode == "invalid":
            output = (
                "Alpha,18.0,32.0,90000,0,estimate\n"
                "Beta,nan,38.0,4500,0,estimate\n"
            )
        else:
            output = all_rows
        return {"ok": geocode_mode != "failed", "returncode":
                7 if geocode_mode == "failed" else 0, "output": output}

    hsb._run = fake_geocode_run
    try:
        geocode_result = hsb.do_geocode(queries)
        check("geocode all rows", geocode_result)
        assert len(geocode_result["rows"]) == 2, geocode_result

        geocode_mode = "missing"
        missing_result = hsb.do_geocode(queries)
        assert not missing_result["ok"], missing_result
        assert missing_result["expected_rows"] == 2, missing_result
        print("[smoke] geocode missing-row rejection: OK")

        geocode_mode = "invalid"
        invalid_result = hsb.do_geocode(queries)
        assert not invalid_result["ok"], invalid_result
        print("[smoke] geocode invalid coordinates: OK")

        geocode_mode = "failed"
        failed_result = hsb.do_geocode(queries)
        assert not failed_result["ok"], failed_result
        assert failed_result["returncode"] == 7, failed_result
        print("[smoke] geocode subprocess failure: OK")
    finally:
        hsb._run = real_run
    assert not list(work.glob(".hsb-geocode-*"))

    personal = work / "personal"
    personal.mkdir()
    config = personal / "hsb-builder.cfg"
    config.write_text("[game_scripts]\n", encoding="utf-8")
    relative_manifest = work / "relative-source.json"
    relative_data = dict(manifest_data)
    relative_data["towns"] = "outputs/towns.json"
    relative_manifest.write_text(json.dumps(relative_data), encoding="utf-8")
    relative_result = hsb.do_run_builder("relative-source.json",
                                         "personal/hsb-builder.cfg")
    assert not relative_result["ok"], relative_result
    assert "absolute" in relative_result.get("error", ""), relative_result

    missing_manifest = work / "missing-source.json"
    missing_data = dict(manifest_data)
    missing_data["towns"] = str(work / "missing-towns.json")
    missing_manifest.write_text(json.dumps(missing_data), encoding="utf-8")
    missing_result = hsb.do_run_builder("missing-source.json",
                                        "personal/hsb-builder.cfg")
    assert not missing_result["ok"], missing_result
    assert not engine_log.exists()
    print("[smoke] run_builder source verification: OK")

    configured_engine = hsb.HSB_ENGINE
    try:
        hsb.HSB_ENGINE = str(work)
        invalid_engine = hsb.do_run_builder("outputs/build.json",
                                            "personal/hsb-builder.cfg")
        assert not invalid_engine["ok"], invalid_engine
        non_executable = work / "not-executable"
        non_executable.write_text("not an engine", encoding="utf-8")
        hsb.HSB_ENGINE = str(non_executable)
        invalid_engine = hsb.do_run_builder("outputs/build.json",
                                            "personal/hsb-builder.cfg")
        assert not invalid_engine["ok"], invalid_engine
        assert not engine_log.exists()
    finally:
        hsb.HSB_ENGINE = configured_engine
    print("[smoke] configured engine validation: OK")

    builder_result = hsb.do_run_builder("outputs/build.json",
                                        "personal/hsb-builder.cfg")
    check("run_builder staging", builder_result)
    staged_dir = personal / "scenario" / "heightmap"
    assert (staged_dir / "map.png").read_bytes() == (work / "map.png").read_bytes()
    assert ((staged_dir / "towns.json").read_bytes() ==
            (work / "outputs/towns.json").read_bytes())
    assert not list(staged_dir.glob(".hsb-stage-*"))
    engine_call = json.loads(engine_log.read_text(encoding="utf-8"))
    assert engine_call["argv"] == ["-e", "-c", str(config)], engine_call
    assert engine_call["manifest"] == str(work / "outputs/build.json"), engine_call
    assert engine_call["cwd"] == str(work), engine_call

    async def mcp_check():
        tools = await hsb.mcp.list_tools()
        tools_by_name = {tool.name: tool for tool in tools}
        names = sorted(tools_by_name)
        expected_names = sorted([
            "build_heightmap", "write_cities_csv", "geocode_towns",
            "build_towns", "write_industries", "write_rivers", "validate",
            "write_manifest", "run_builder",
        ])
        assert names == expected_names, names
        print(f"[smoke] list_tools: OK ({len(names)} tools)")

        run_schema = tools_by_name["run_builder"].input_schema
        assert set(run_schema["properties"]) == {"manifest", "config", "timeout"}
        assert "engine" not in run_schema["properties"]
        town_properties = tools_by_name["build_towns"].input_schema["properties"]
        assert {"sample_only", "strict", "timeout"} <= set(town_properties)
        validate_properties = tools_by_name["validate"].input_schema["properties"]
        assert {"scenario", "strict", "timeout"} <= set(validate_properties)
        manifest_properties = tools_by_name["write_manifest"].input_schema["properties"]
        assert {"height", "rotation", "seed", "industry_search_radius",
                "timeout"} <= set(manifest_properties)
        bounds_schema = json.dumps(tools_by_name["validate"].input_schema)
        assert '"maximum": 90' in bounds_schema, bounds_schema
        assert '"minimum": -180' in bounds_schema, bounds_schema
        print("[smoke] tool schemas: OK")

        valid_call = await hsb.mcp.call_tool("validate", {
            "heightmap": "map.png", "towns": "outputs/towns.json",
            "industries": "inputs/industries.json",
            "bounds": {"lat_min": 10.0, "lat_max": 20.0,
                       "lon_min": 30.0, "lon_max": 40.0},
            "cities": "inputs/cities.csv", "rivers": "inputs/rivers.json",
            "scenario": "built.scn", "industry_types": "Farm", "strict": True,
        })
        assert not valid_call.is_error, valid_call
        print("[smoke] call_tool(validate): OK")

        try:
            await hsb.mcp.call_tool("validate", {
                "heightmap": "map.png", "towns": "outputs/towns.json",
                "industries": "inputs/industries.json",
                "bounds": {"lat_min": 91.0, "lat_max": 20.0,
                           "lon_min": 30.0, "lon_max": 40.0},
            })
        except ToolError as exc:
            assert "Traceback" not in str(exc), exc
            print("[smoke] invalid validator schema: OK")
        else:
            raise AssertionError("invalid bounds were accepted")

    asyncio.run(mcp_check())


def main():
    with tempfile.TemporaryDirectory(prefix="hsb-mcp-smoke-") as temporary:
        temporary_path = Path(temporary)
        run_smoke(temporary)
        print("[smoke] ALL CHECKS PASSED")
    assert not temporary_path.exists(), temporary_path


if __name__ == "__main__":
    main()
