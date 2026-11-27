# AnyPS5 for Marvel's Wolverine on PC

This is [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) with 29 patches used to run **Marvel's Wolverine (PS5, PPSA03671, version 1.05)** on Windows.

**What AnyPS5 does here:**
- Its **relinker** converts the game's `eboot.bin` into a native Windows `Wolverine.exe`.
- Its **system libraries** (`libs\*.prx`) stand in for the PS5's own.
- `libAstroHybridHost.prx` (used as `astrohybrid_host.dll`) connects the game to the Kyty GPU engine.

**Graphics** are not drawn by AnyPS5's own renderer. They go through Kyty (`kyty_gpu.dll`) from [Wolverine-PC-Kyty](https://github.com/bc160201983/Wolverine-PC-Kyty).

**No game files are included.** You need your own dump of the game.

The original AnyPS5 README is in [docs/UPSTREAM-README.md](docs/UPSTREAM-README.md).

## The three parts

| Repo | Makes |
|---|---|
| **AnyPS5-Wolverine** (this one) | `relinker.exe`, `Wolverine.exe` (from your eboot), `libs\*.prx`, `astrohybrid_host.dll` |
| [Wolverine-PC-Kyty](https://github.com/bc160201983/Wolverine-PC-Kyty) | `kyty_gpu.dll`: PS5 graphics (Agc) to Vulkan; this is where the fps, crash and stutter work lives |
| Wolverine-PC (coming) | Build scripts, run-folder setup and launcher settings that put the parts together |

The guest-to-Kyty forwarders (`libSceAgc`, `libSceAgcDriver`, `libSceVideoOut` in `libs\`) are generated from `src/hybrid/kytyGpuNids.inc` in the Kyty repo.

## What the patches change

These are on top of AnyPS5 `319621ba2e`. The Kyty bridge is active when the game runs with `APS5_GPU_ENGINE=kyty`. Many of the speed-ups are behind the named `APS5_*` switches.

**Kyty bridge (Wolverine runs on these):**
- `libkernel`: hands guest memory to Kyty's GPU stack (`APS5_GPU_ENGINE=kyty`).
- `libAstroHybridHost`:
  - publishes Kyty's window input to `libScePad`;
  - takes pad motion sensors from Kyty;
  - ends the process cleanly when Kyty's window closes.
- `libScePad`: DualSense trigger-effect state from the game, with pad output sent to Kyty.
- `libAudioOut` / `libSaveData`:
  - wireless DualSense audio forwarded to Kyty;
  - `sceSaveDataTransferringMount` exported.
- `libc`: optionally keeps a CUDA address window free in the guest arena.
- Wolverine fixes:
  - an AudioOut2 deadlock;
  - APR/AMPR file-reader fixes;
  - thread dumps in crash reports;
  - Game Presets priority (`APS5_GAME_PRESET_PRIORITY`).

**Speed-ups in AnyPS5's own Agc renderer and libc** (useful for other games; Wolverine draws through Kyty):
- draw snapshots in resizable-BAR memory;
- GPU-produced buffers bound in place and read from device-local shadows;
- DMA_DATA/GDS stores recorded on the GPU instead of draining;
- write alarms on resident guest pages instead of `GetWriteWatch` polling;
- compute lanes on a second Vulkan queue (`APS5_COMPUTE_LANE`, `APS5_COMPUTE_QUEUE`);
- completion-label-only syncs (`APS5_SYNC_COMPLETION_LABELS_ONLY`);
- a texture-cache budget floor (`APS5_SAMPLED_BUDGET_FLOOR_MIB`);
- pool shedding over the VRAM budget;
- rebased draw-stage reuse;
- recompiler memo fast path;
- `APS5_TRACE_MARKERS` (pass names with draw/dispatch counts per frame).

The full list is in `git log 319621ba2e..main`.

## Build (Windows)

What you need:
- **WinLibs GCC 15.2.0 posix-seh UCRT r7**, from [this exact zip](https://github.com/brechtsanders/winlibs_mingw/releases/download/15.2.0posix-14.0.0-ucrt-r7/winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-14.0.0-r7.zip). Its SHA-256 is `CB2FBAD6162540CDF5E1FACDCE08D4DAC359E8CF64F7F696A99274291763B815`.
- Python 3 and Git.

Put the WinLibs `mingw64\bin` folder first on PATH **in this shell only**; its bundled CMake and Ninja are the ones used. If the FFmpeg download fails with a TLS error, set `SSL_CERT_FILE` to a CA bundle. Then build:

```
git clone https://github.com/bc160201983/AnyPS5-Wolverine.git
cd AnyPS5-Wolverine
git submodule update --init --recursive --depth 1
cmake -S . -B _build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DPython3_EXECUTABLE=<path to python.exe>
cmake --build _build --parallel
cmake --build _build --target libs --parallel
```

Outputs:
- `_build\core\relinker\relinker.exe`
- `_build\core\libs\libs\*.prx`, including `libAstroHybridHost.prx`

## Make Wolverine.exe

Do this in a **copy** of your dump's folder (the relinker writes `*.guest.prx` modules next to the game files):

```
relinker --windows --registry --module-dir . eboot.bin <RunFolder>\Wolverine.exe
```

Then assemble the run folder:

| Copy | To |
|---|---|
| `libs\*.prx` | `<RunFolder>\libs\` |
| `libAstroHybridHost.prx` | `<RunFolder>\astrohybrid_host.dll` |
| `kyty_gpu.dll` from Wolverine-PC-Kyty | `<RunFolder>\` |
| your `app0` | `<RunFolder>\app0\` |

Start it with the launcher settings in the Wolverine-PC-Kyty README.

## Credits and license

- AnyPS5 by boykopovar and contributors.
- Patches by faycal and BC160201983.

GPL-2.0, see [LICENSE](LICENSE). This is an unofficial fan project, not affiliated with Insomniac Games, Sony or Marvel.
