#!/usr/bin/env python3
"""Functional checks for the OSM projection and network derivation.

Run: python3 tests/test_osm_network.py
"""
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_script(name):
    path = os.path.join(ROOT, "scripts", name)
    spec = importlib.util.spec_from_file_location(name.replace("-", "_")[:-3], path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_reference(path, key, rows):
    with open(path, "w", encoding="utf-8") as f:
        f.write('{\n"generated_from": [],\n"%s": [\n' % key)
        f.write(",\n".join(json.dumps(row, separators=(",", ":"), sort_keys=True)
                           for row in rows))
        f.write("\n]\n}\n")


def quiet(call, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return call(*args, **kwargs)


def test_projection():
    osm = load_script("build-osm-network.py")
    osm.configure(47.0, 55.5, 4.0, 15.5, 4096)
    x, y = osm.project(53.55, 9.99)
    assert abs(x - 939.44) < 0.1, (x, y)
    assert abs(y - 2132.96) < 0.1, (x, y)
    lat, lon = osm.unproject(x, y)
    assert abs(lat - 53.55) < 1e-9 and abs(lon - 9.99) < 1e-9, (lat, lon)
    assert osm.project(55.5, 4.0) == (0.0, 0.0)
    assert osm.project(47.0, 15.5) == (4095.0, 4095.0)


def test_network_derivation():
    osm = load_script("build-osm-network.py")
    with tempfile.TemporaryDirectory() as tmp:
        ref = os.path.join(tmp, "ref")
        out = os.path.join(tmp, "out")
        os.makedirs(ref)
        os.makedirs(out)
        write_reference(os.path.join(ref, "osm-road.json"), "ways", [
            {"id": 1, "kind": "way", "class": "motorway", "ref": "A 1",
             "name": "A 1", "nodes": [[51.0, 9.0], [51.1, 9.1], [51.2, 9.2]],
             "km": 20.0, "service": ""},
            {"id": 2, "kind": "way", "class": "motorway", "ref": "A 1",
             "name": "A 1", "nodes": [[51.2, 9.2], [51.3, 9.3]],
             "km": 10.0, "service": ""},
        ])
        write_reference(os.path.join(ref, "osm-rail.json"), "ways", [
            {"id": 10, "kind": "way", "class": "rail", "ref": "1",
             "name": "Teststrecke", "nodes": [[51.0, 9.0], [51.3, 9.3]],
             "km": 30.0, "usage": "main", "service": ""},
        ])
        write_reference(os.path.join(ref, "osm-stations.json"), "stations", [
            {"id": 100, "kind": "node", "type": "station", "name": "Teststadt Hbf",
             "lat": 51.25, "lon": 9.75,
             "tags": {"railway": "station", "railway:station_category": "3"}},
            {"id": 101, "kind": "node", "type": "halt", "name": "Teststadt Dorf",
             "lat": 51.26, "lon": 9.76, "tags": {"railway": "halt"}},
        ])
        towns = os.path.join(tmp, "towns.json")
        with open(towns, "w", encoding="utf-8") as f:
            json.dump([{"name": "Teststadt", "population": 1000, "city": True,
                        "x": 0.5, "y": 0.5}], f)

        args = ["--reference-dir", ref, "--out-dir", out, "--towns", towns,
                "--lat-min", "47", "--lat-max", "55.5",
                "--lon-min", "4", "--lon-max", "15.5", "--size", "4096"]
        assert quiet(osm.main, args) == 0

        with open(os.path.join(out, "autobahn-osm.json"), encoding="utf-8") as f:
            autobahn = json.load(f)
        with open(os.path.join(out, "rail-osm.json"), encoding="utf-8") as f:
            rail = json.load(f)
        with open(os.path.join(out, "stations-osm.json"), encoding="utf-8") as f:
            stations = json.load(f)
        with open(os.path.join(out, "osm-import.json"), encoding="utf-8") as f:
            provenance = json.load(f)

        assert len(autobahn) == 1 and len(autobahn[0]["waypoints"]) >= 2
        assert len(rail) == 1 and rail[0]["name"] == "Teststrecke"
        assert len(stations) == 1
        assert stations[0]["town"] == "Teststadt"
        assert stations[0]["name"] == "Teststadt Hbf"
        assert provenance["counts"]["motorway_corridors"] == 1
        assert provenance["projection"]["latitude"].startswith("linear")

        for corridor in autobahn + rail:
            for lat, lon in corridor["waypoints"]:
                assert 47.0 <= lat <= 55.5 and 4.0 <= lon <= 15.5

        assert quiet(osm.main, args + ["--check"]) == 0
        with open(os.path.join(out, "autobahn-osm.json"), "w", encoding="utf-8") as f:
            f.write("[]\n")
        assert quiet(osm.main, args + ["--check"]) == 1


def test_manifest_and_validation_wiring():
    from PIL import Image

    osm = load_script("build-osm-network.py")
    manifest_script = load_script("build-manifest.py")
    validate_script = load_script("validate-scenario.py")
    with tempfile.TemporaryDirectory() as tmp:
        ref = os.path.join(tmp, "ref")
        out = os.path.join(tmp, "out")
        os.makedirs(ref)
        os.makedirs(out)
        write_reference(os.path.join(ref, "osm-road.json"), "ways", [
            {"id": 1, "kind": "way", "class": "motorway", "ref": "A 1",
             "name": "A 1", "nodes": [[51.0, 9.0], [51.1, 9.1], [51.2, 9.2]],
             "km": 20.0, "service": ""},
        ])
        write_reference(os.path.join(ref, "osm-rail.json"), "ways", [
            {"id": 10, "kind": "way", "class": "rail", "ref": "1",
             "name": "Teststrecke", "nodes": [[51.0, 9.0], [51.3, 9.3]],
             "km": 30.0, "usage": "main", "service": ""},
        ])
        write_reference(os.path.join(ref, "osm-stations.json"), "stations", [
            {"id": 100, "kind": "node", "type": "station", "name": "Teststadt Hbf",
             "lat": 51.25, "lon": 9.75,
             "tags": {"railway": "station", "railway:station_category": "3"}},
        ])
        towns = os.path.join(tmp, "towns.json")
        with open(towns, "w", encoding="utf-8") as f:
            json.dump([{"name": "Teststadt", "population": 1000, "city": True,
                        "x": 0.5, "y": 0.5}], f)
        industries = os.path.join(tmp, "industries.json")
        with open(industries, "w", encoding="utf-8") as f:
            f.write("[]\n")
        heightmap = os.path.join(tmp, "map.png")
        Image.new("L", (64, 64), 128).save(heightmap)

        assert quiet(osm.main, [
            "--reference-dir", ref, "--out-dir", out, "--towns", towns,
            "--lat-min", "47", "--lat-max", "55.5",
            "--lon-min", "4", "--lon-max", "15.5", "--size", "4096",
        ]) == 0

        manifest_path = os.path.join(tmp, "manifest.json")
        assert quiet(manifest_script.main, [
            "--heightmap", heightmap, "--towns", towns,
            "--industries", industries, "--osm", out,
            "--lat-min", "47", "--lat-max", "55.5",
            "--lon-min", "4", "--lon-max", "15.5",
            "--competitors", "2", "--out", manifest_path,
        ]) == 0
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
        assert manifest["autobahnen"] == os.path.join(out, "autobahn-osm.json")
        assert manifest["rail"] == os.path.join(out, "rail-osm.json")
        assert manifest["stations"] == os.path.join(out, "stations-osm.json")

        assert quiet(validate_script.main, [
            "--heightmap", heightmap, "--towns", towns,
            "--industries", industries, "--osm", out,
            "--lat-min", "47", "--lat-max", "55.5",
            "--lon-min", "4", "--lon-max", "15.5",
            "--scenario", os.path.join(tmp, "missing.scn"),
        ]) == 0


if __name__ == "__main__":
    test_projection()
    test_network_derivation()
    test_manifest_and_validation_wiring()
    print("osm tests OK")
    sys.exit(0)
