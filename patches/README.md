# Patches — HSB (apply order 01)

Apply: `sh scripts/apply-patches.sh` (or
`git -C openttd-upstream apply ../patches/0*.diff`).
Revert: `git -C openttd-upstream apply --reverse ../patches/01-headless-scenario-builder.diff` (or clean the patch-created `src/hsb_builder*` and `src/tests/hsb_builder.cpp` files).
(check `git -C openttd-upstream status` — keep the submodule pristine).

* `01-headless-scenario-builder.diff` — `src/hsb_builder.{h,cpp}`, math header,
  Catch2 tests, hook in `GameLoop`, CMake entries, and the counter-clockwise
  town mapping fix. `HSB_BUILD=<manifest> openttd -e` builds a scenario
  headlessly (heightmap → waters → towns → industries → start year → save →
  exit). Source of truth is `builder/`; regenerate with
  `sh scripts/gen-patch.sh` and verify parity with
  `sh scripts/gen-patch.sh --check`. Run `sh scripts/test-builder.sh` for the
  builder unit suite.

Scope: the builder intentionally does NOT lock menus, branding, or settings —
it is a pure content pipeline.
