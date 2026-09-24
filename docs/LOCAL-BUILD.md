# Local build + headless run (no sudo required)

Proven on Ubuntu 24.04 (GCC 13, CMake 3.28, Python 3.12) via the GTTD origin
project. `build/` is git-ignored; `/tmp/deps` + personal dir are machine state.

## 1. Dev-stack bootstrap (apt download, no root)

```sh
mkdir -p /tmp/deps && cd /tmp/deps  # work OUTSIDE the repo (no .debs in git!)
apt download -o=Debug::NoLocking=1 libsdl2-dev libpng-dev libfreetype-dev zlib1g-dev \
  libasound2-dev libpulse-dev libsamplerate0-dev libx11-dev libxext-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxrandr-dev libxss-dev libdrm-dev libgbm-dev libwayland-dev \
  libxkbcommon-dev libdecor-0-dev libbrotli-dev libbz2-dev liblzma-dev
for d in *.deb; do dpkg -x "$d" /tmp/deps/root; done
```

Debian `.so` symlinks dangle (they target snapshot versions); repoint them:

```sh
python3 - <<'EOF'
import os, glob
d = '/tmp/deps/root/usr/lib/x86_64-linux-gnu'
for link in glob.glob(d + '/*.so'):
    if not os.path.islink(link): continue
    t = os.readlink(link)
    if os.path.exists(os.path.join(d, t)) or os.path.exists(t): continue
    c = sorted(glob.glob('/usr/lib/x86_64-linux-gnu/%s.so.*' % os.path.basename(link)[:-3]))
    if c: os.remove(link); os.symlink(c[0], link); print('fixed', os.path.basename(link))
EOF
ln -sf ../x86_64-linux-gnu/SDL2/_real_SDL_config.h /tmp/deps/root/usr/include/SDL2/_real_SDL_config.h
```

(With sudo, just `apt install` the same `-dev` packages.)

## 2. Configure + build

```sh
git submodule update --init --depth 1
sh scripts/fetch-upstream.sh
sh scripts/apply-patches.sh   # or --check-only for CI
cmake -S openttd-upstream -B build/upstream -DCMAKE_BUILD_TYPE=Release \
  -DOPTION_DEDICATED=OFF -DCMAKE_PREFIX_PATH=/tmp/deps/root/usr \
  "-DCMAKE_EXE_LINKER_FLAGS=-L/tmp/deps/root/usr/lib/x86_64-linux-gnu -lbrotlidec -lbrotlicommon -lbz2"
cmake --build build/upstream -j$(nproc)   # ~30-45 min first time, incremental after
```

Notes: CMake may resolve freetype to the static archive (hence the explicit
brotli/bz2 libs above). Keep the submodule pristine: verify with
`git -C openttd-upstream status` before committing (patches are tested via
`git apply`, then reverted).

## 3. Headless scenario build

```sh
# game data where the engine finds it (personal dir == -c file's dir!)
cp /tmp/map-4096.png /tmp/towns.json <personal-dir>/scenario/heightmap/
# config WITH NewGRFs + params + GS, stored INSIDE the personal dir:
#   [game_scripts] my-gs =
#   [newgrf] <grfid> = <params>
python3 scripts/build-manifest.py --help  # bounds + inputs -> /tmp/build.json
HSB_BUILD=/tmp/build.json DISPLAY=:99 SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
  ./build/upstream/openttd -e -c <personal-dir>/hsb-builder.cfg
# => scenario/output.scn in ~100 s (4096). Needs Xvfb (SDL x11); dummy audio
# is REQUIRED (without it the window never maps under Xvfb).
```

Rules learned the hard way: `-c` location defines the personal dir (a `-c`
in /tmp reads /tmp/scenario/...); keep the submodule pristine
(`git apply` patches, then `checkout -- .`); FixSlopes() must run after any
direct terrain edits or town road-building asserts; newgame settings (not
game settings) survive GenerateWorld's reset.
