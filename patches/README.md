# Patches — HSB (apply order 01)

Apply: `sh scripts/apply-patches.sh` (or
`git -C openttd-upstream apply ../patches/0*.diff`).
Revert: `git -C openttd-upstream apply --reverse ../patches/01-headless-scenario-builder.diff` (or clean the patch-created `src/hsb_builder*` and `src/tests/hsb_builder.cpp` files).
(check `git -C openttd-upstream status` — keep the submodule pristine).

* `01-headless-scenario-builder.diff` — `src/hsb_builder.{h,cpp}`, the two
  math/network headers, Catch2 tests, and five upstream edits:
  * `src/openttd.cpp` — the `HSB_BUILD` hook in `GameLoop`, plus `HSB_FAST=1`
    for test runs.
  * `src/rail.cpp` — `HasRailTypeAvail` accepts the editor and worldgen company
    cases. `HasRoadTypeAvail` has always had this handled; without it
    `ValParamRailType()` reads through a company that does not exist yet and
    building the first rail tile of a scenario segfaults.
  * `src/saveload/misc_sl.cpp` — `_hsb_start_view_tile`, the tile the builder
    wants the scenario to open on, written into the `.scn` as data so a **stock,
    unpatched** engine opens on a town instead of the empty map corner.
  * `src/genworld.cpp` — the counter-clockwise town mapping fix.
  * CMake entries in `src/` and `src/tests/`.

  `HSB_BUILD=<manifest> openttd -e` builds a scenario headlessly
  (heightmap → waters → towns → industries → roads → rail → airports → docks →
  fleet → start year → save → exit). Source of truth is `builder/`; regenerate
  with `sh scripts/gen-patch.sh` and verify parity with
  `sh scripts/gen-patch.sh --check`. Run `sh scripts/test-builder.sh` for the
  builder unit suite.

Scope: the builder intentionally does NOT lock menus, branding, or settings —
it is a pure content pipeline.

## The patch is the source of truth, not the submodule worktree

Never `git checkout` a file this series creates.

`src/hsb_builder.cpp` and its siblings are *added* by the patch and sit in the
submodule index as intent-to-add entries with an **empty blob**, because the
workflow is "apply the patch, build, regenerate the patch, never commit inside
the submodule". An intent-to-add entry holds no content, so:

```
git -C openttd-upstream checkout -- src/hsb_builder.cpp   # -> 0 bytes
```

That fails quietly — `checkout` reports success, the diff afterwards is empty,
and the build only complains at link time with an undefined reference.

`scripts/gen-patch.sh` avoids the trap entirely: when it finds its own previous
patch applied, it reverses it before regenerating. To undo an edit by hand,
re-apply the patch instead:

```
rm -f src/hsb_builder.cpp
git apply --include='src/hsb_builder.cpp' ../patches/01-headless-scenario-builder.diff
```

A full re-apply is the safer of the two when in doubt: it restores exactly what
shipped, and `wc -l` plus a build confirm it.
