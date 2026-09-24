# Patches — HSB (apply order 01)

Apply: `sh scripts/apply-patches.sh` (or
`git -C openttd-upstream apply ../patches/0*.diff`).
Revert: `git -C openttd-upstream checkout -- .`
(check `git -C openttd-upstream status` — keep the submodule pristine).

* `01-headless-scenario-builder.diff` — `src/hsb_builder.{h,cpp}` + hook in
  `GameLoop` + CMake entry. `HSB_BUILD=<manifest> openttd -e` builds a
  scenario headlessly (heightmap → waters → towns → industries →
  start year → save → exit). Source of truth is `builder/`; regenerate with
  `sh scripts/gen-patch.sh`. See `docs/FORMATS.md` + `docs/SCENARIO.md`.

Scope: the builder intentionally does NOT lock menus, branding, or settings —
it is a pure content pipeline. Forks like GTTD layer lock-down patches
on top.
