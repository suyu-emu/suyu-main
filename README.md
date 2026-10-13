# suyu

<h1 align="center">
  <br>
  <img src="dist/suyu.svg" alt="suyu" height="128">
  <br>
  <b>suyu</b>
  <br>
</h1>

<h4 align="center">
A Nintendo Switch Emulation, Recompilation, Development and Launcher Toolkit — based on <a href="https://git.eden-emu.dev/eden-emu/eden">Eden</a>, which itself descends from yuzu.
</h4>

<p align="center">
  <a href="#status">Status</a> |
  <a href="#static-recompilation">Static recompilation</a> |
  <a href="#performance">Performance</a> |
  <a href="docs/releases/v14.1.0.md">Changes in v14.1.0</a> |
  <a href="#building">Building</a> |
  <a href="#license">License</a>
</p>

---

> **Current release: [suyu v14.1.0](https://github.com/suyu-emu/suyu-main/releases/tag/v14.1.0)**
>
> [`suyu-emu/suyu-v0.0.4`](https://github.com/suyu-emu/suyu-v0.0.4) is a public
> archive and no further development was planned there. This repository picks it up
> from commit `d1d09321d7` and continues development, now at **v14.1.0**.
>
> Tagged builds identify their exact release, such as `suyu v14.1.0`.
> Development snapshots identify their commit and branch. See
> [PROVENANCE.md](PROVENANCE.md) for the repository lineage.
>
> The default development branch is `mk8-recomp`. Recompiler work is also maintained in
> [mk8-recomp](https://github.com/dougchansan/mk8-recomp) — a project statically
> recompiling Switch titles using this recompiler. General emulator and launcher
> fixes are maintained here alongside that work.

## About

suyu is a Nintendo Switch emulator and AArch64 native recompiler written in C++. It can run decrypted Switch titles using either:

- **Ordinary emulation** — Dynarmic JIT CPU execution with suyu's GPU, audio and service emulation.
- **Static or Hybrid recompilation** — ahead-of-time compilation of AArch64 game code, using the same GPU, audio and HLE services. Hybrid permits JIT fallback; Static requires compiled coverage.

Based on [Eden](https://git.eden-emu.dev/eden-emu/eden), with suyu's own improvements to UI, recompiler, and platform support.

## Status

**v14.1.0 corrects NVIDIA pipeline-library rendering and macOS character geometry**,
following v14.0.1. Versions use major.minor.patch numbering. Official packages are
built by the [tagged release workflow](https://github.com/suyu-emu/suyu-main/actions/workflows/release.yml),
which checks platform builds and final package contents before publishing.

Graphics pipeline libraries and Fast GPU Time are **off by default**. Graphics →
Advanced exposes the graphics hacks and Vulkan extension controls, supports global
and per-game settings, and scrolls on smaller windows. Explicit user opt-ins are
preserved. See the [release notes](docs/releases/v14.1.0.md).

### Released platforms

| Target | Official downloads | Current validation |
| --- | --- | --- |
| Windows x86-64 | GUI/CLI ZIP; libretro ZIP | Radeon RX 9070 XT: bounded MK8D/TOTK Static checks with optional graphics settings off and on |
| Linux x86-64 | Tarball; Ubuntu 24.04 AppImage; libretro tarball | RTX 3090: bounded MK8D/TOTK Static checks; package/source and AppImage startup checks |
| macOS arm64 | Application ZIP; libretro ZIP | Apple M5: TOTK JIT and MK8D/TOTK native Static opening checks using MoltenVK |
| Android arm64 | APK; libretro ZIP | Platform build and package checks; real-device gameplay remains unqualified |

[Download v14.1.0](https://github.com/suyu-emu/suyu-main/releases/tag/v14.1.0).
Project, dependency and AppImage corresponding-source archives accompany the
downloads. Official packages contain no games, keys or firmware.

iOS has experimental no-JIT source/probe support. FreeBSD and Linux arm64 have
platform code paths, but none of these has an official package or a qualifying
job in the current release workflow. Their presence in the source does not imply
the same support level as the released targets.

Windows Static opening-scene checks passed for Mario Kart 8 Deluxe, Super Mario
Odyssey, Tears of the Kingdom and Xenoblade Chronicles 3: rendering and input
response were reviewed, with clean exits and zero JIT transitions. These are
bounded compatibility checks; see [Performance](#performance) for their FPS scope.

## Static recompilation

**File > Export Game** turns a game into its own package. On Windows, a **Build**
export is a program with your settings, optional shader cache and automatic
controller setup, and can be added to Steam directly. Linux and macOS exports are
Source only. The default Reference package reads your original game file at launch
using the keys installed in suyu. The optional Portable package copies encrypted
game, update and DLC files into the local export, bound to the same console's keys;
it still requires those keys. Keys and firmware are never copied into an export.
Hybrid and Static exports, including Source output, contain translated game code.
An export is a local package; official suyu releases contain no game material. See the
[Export Game guide](docs/user/GameExport.md).

Static and Hybrid execution remain experimental. Windows opening checks cover
four games; native Linux and macOS checks cover MK8D and TOTK. Full playthroughs,
additional drivers and runtime-generated game code need further validation.
Historical measurements are listed separately under [Performance](#performance).

| Mode (in Export Game order) | Purpose |
|---|---|
| suyu Dynarmic JIT (Baseline) | Default. Dynamic compilation; the most compatible. |
| suyu Hybrid JIT + AOT | Static code with JIT fallback. Performance varies by game; compare it with the Dynarmic JIT export. |
| suyu static AOT (Experimental) | Ahead-of-time AArch64 code with suyu HLE; for a host with Dynarmic entirely absent, build with `-DSUYU_NO_JIT=ON`. |

Builds configured with `-DSUYU_NO_JIT=ON` leave Dynarmic out entirely; releases no longer ship separate no-JIT downloads. Selecting static export mode in an ordinary host is a separate fallback policy; it does not remove the dynamic compiler from that host. No-JIT hosts require compiled coverage and cannot run unsupported AArch32 or runtime-generated code.

**Re-export Static and Hybrid games for v14.1.0.** Existing exports embed their own
CPU/HLE host; updating the GUI alone does not apply these graphics fixes. Generated images use ABI 6 by
default, with page-table memory access, guard-generation checks and floating-point
fast paths. Compatibility output can use ABI 5. Automatic title bundles validate
manifests, image hashes, ABI and instruction bytes. Hosted library launches use the
current bundle rather than stale detached launchers.

A strict-static MK8D v4.0.0 TAS replay reaches a rendered, controllable race segment with zero JIT fallbacks. Full-race validation remains open. Tested paths are evidence of compatibility, not a guarantee for all titles or instructions.

See [current release notes](docs/releases/v14.1.0.md) and the [campaign and regression safeguards](docs/static-campaign.md). Build/test scripts and synthetic instruction suites are maintained in [mk8-recomp](https://github.com/dougchansan/mk8-recomp).

## Performance

### v14.1.0 graphics validation

Native Static opening replays completed with normal exits, all four translated
modules active, zero JIT transitions and zero unresolved imports for MK8D 4.0.0
(6,001 frames) and TOTK 1.4.3 (12,001 frames) on Linux RTX 3090, Windows Radeon
RX 9070 XT and macOS Apple M5. Linux checks verified the NVIDIA fix; Windows
Radeon checks covered the same change as a regression test. The later Mac cases
also verified the MoltenVK fix. The final Mac-only change rebuilt
successfully on Windows. These checks reviewed captured rendering and do not
establish full-game compatibility or a release-to-release performance gain.

On Apple M5, later MK8D race samples were around 60 FPS. TOTK cave samples were
around 16 FPS with Fast GPU Time off and 9–10 FPS with Medium enabled. These are
observations from separate functional sessions, not a controlled benchmark.
Fast GPU Time remains off by default. This MoltenVK driver does not support
graphics pipeline libraries, so requests use the normal pipeline fallback.

### v14.0.1 functional observations

These Windows Static opening sessions used a Ryzen 9 9950X3D and Radeon RX 9070 XT
(AMD driver 2.0.395), with pipeline libraries off. These were functional playtests,
not controlled benchmarks. The following averages include loading, menus and shader activity;
zero-FPS samples and dips are retained. They describe these sessions and do not
establish stable frame rates, a JIT comparison or a release-to-release speedup.

| Game | Version | Opening area | Whole-session mean FPS | Minimum FPS |
| --- | --- | --- | ---: | ---: |
| Mario Kart 8 Deluxe | 4.0.0 | Mario Kart Stadium | 55.99 | 0 |
| Super Mario Odyssey | 1.3.0 | Cap Kingdom | 47.16 | 0 |
| Tears of the Kingdom | 1.4.3 | Beneath Hyrule Castle | 28.14 | 0 |
| Xenoblade Chronicles 3 | 1.0.0 | Opening battle and Everblight Plain | 27.92 | 0 |

The sampled MK8D race window averaged **59.99 FPS**, with a **59.94 FPS** minimum.
The reviewed Odyssey gameplay window averaged **56.94 FPS**, with a **2.50 FPS**
minimum. TOTK and XC3 whole-session medians were **29.98 FPS**. These windows
have different boundaries; shader creation and system load can still cause
stutters. The checks confirmed rendering, controls and clean shutdown, rather
than completion of an entire race or game.

### Historical measurements

- [v0.0.11 performance notes](docs/releases/v0.0.11.md#performance) report MK8D
  Windows Static improving from 23.7 to 56.1 FPS with the documented compiler
  and hardware setup.
- [v0.0.13 notes](docs/releases/v0.0.13.md) report uncapped TOTK 1.4.3 gains from
  largest-L3-cache affinity on a 9950X3D (about 40% Static and 60% JIT), plus
  earlier Static and Hybrid improvements.

Those results belong to their recorded builds and settings. They are not new
v14.1.0 benchmark results. Controlled comparisons need matching games, saves,
mods, caches and work, with interleaved runs on an idle machine; see
[the measurement safeguards](docs/static-campaign.md).

### Further development

The main validation gaps are longer races and playthroughs, more GPU/driver
coverage, real Android hardware, and qualification of experimental platforms.
Native export packaging beyond Windows and controlled recompiler performance
comparisons remain areas for further work.

## Changes in recent updates

v14.1.0 fixes corrupted NVIDIA frames when pipeline libraries are enabled and
stretched character geometry on macOS MoltenVK. v14.0.1 adds safer graphics
defaults, complete advanced graphics controls and a working Fast GPU Time switch.
v0.0.14 fixed startup/export-close crashes and Source
exports with complex title names, corrected release version display, and added
the Linux AppImage. See the [release history](docs/releases/) for details.

Earlier correctness fixes and recompiler additions include:

### Fixes

- **Installed updates and DLC in NAND were never indexed.** `GetFileAtID` tried
  eight storage-layout variants but skipped every odd index except 7, so the
  `.cnmt.nca` suffix was only ever looked for at the cache root — never inside a
  `000000XX/` directory, which is exactly where meta NCAs are stored and what
  `InstallEntry` writes. Every meta NCA in NAND was therefore unreachable and no
  installed update or DLC ever entered the cache, silently: a miss is
  indistinguishable from nothing being installed, which is why the frontend's
  installed-title listing reported zero. A second defect behind it let an older
  update overwrite a newer one, because the metadata map is keyed by title id
  with no version comparison — now the higher `GetTitleVersion()` wins.

- **Service handler registration dropped most commands.** A
  `FunctionInfoTyped<T>` array was walked through a `FunctionInfoBase*` with a
  different member layout. `sizeof()` agrees, so a size assertion passes and
  tells you nothing, but every element after the first was read from the wrong
  offset. `IpcController` registered 2 of its 6 handlers;
  `QueryPointerBufferSize` was among the lost, and it is part of CMIF session
  setup — so titles stalled in early service initialisation.

- **RomFS registration was silently dropped.** `emplace` where
  `insert_or_assign` was meant, so re-registration kept the stale entry and the
  title panicked on boot.

- **AOT image dispatch resolved every PC to the wrong module.** Double base
  subtraction made every lookup underflow, and a four-entry module table
  mismapped any title with more than one subsdk.

- **The AOT exporter read the base ExeFS, not the update's.** `PatchManager`
  replaces the ExeFS wholesale when an update is present, so the exported image
  diverged from live execution on any updated title.

### Additions

- **AArch64 → C recompiler work.** Exclusives now route through
  `Core::ExclusiveMonitor` (previously a plain load/store with `STXR` always
  reporting success, which makes every compare-and-swap non-atomic under real
  threads); FPCR/FPSR are modelled; the counter and `CTR_EL0` are read from the
  emulator's own sources so the two engines cannot disagree across a transition.
  Plus EXTR/ROR, ADC/SBC, LDPSW, exclusive pair forms, PRFM, and the DC
  cache-maintenance family.

- **A build with no dynamic recompiler in it.** `-DSUYU_NO_JIT=ON` drops
  dynarmic from every target. The exclusive monitor, which every process builds
  regardless of engine and which dynarmic previously owned the only
  implementation of, now has a standalone one; `ArmRecomp` holds the
  `Core::ExclusiveMonitor` interface rather than dynarmic's implementation of
  it. See [Static recompilation](#static-recompilation).

- **Static and runtime coverage instrumentation** — per-module JSON of
  emitted/unhandled counts, and runtime histograms of blocks executed,
  transitions by cause, unimplemented opcodes and SVCs.

- **`suyu-cmd --probe-isa-list`** reports each title's CPU architecture without
  booting it, reading the update's NPDM as well as the base's. An update can
  change the answer: a title can ship a 32-bit base and a later 64-bit update.

- **Diagnostics** — the NPDM log line carries a content hash, because size is not
  an identity - two updates of one title can share a `main.npdm` size while
  differing in architecture - and `PatchExeFS` names which provider slot
  answered for an update.

Full change set:

```
git diff d1d09321d7ab84252291e05b3efbc8a8dfa57481..mk8-recomp
```

## Legal Notice

suyu is a GPLv3 program, which allows fully free redistribution of its source code and releases liability of its authors for how this software is used as stated in Section 15 and 16.

The suyu Emulator program does not circumvent Nintendo's technological protection measures (TPMs) as the user is required to provide both the Nintendo Switch software & the encryption keys for these games, and the suyu Emulator uses a mode of the Advanced Encryption Standard (AES), an open encryption standard established by the US NIST, along with the encryption keys that the user themselves must lawfully acquire, to decrypt the software. As the standard is public and available to use by all, it does not constitute as the Digital Market Copyright Act's (DMCA) definition of "circumventing a technological measure" as defined in Section 1201(a)(3).

The suyu Emulator also falls under the exemptions stated in Section 1201(f) of the DMCA as this software was created for the purposes of reverse engineering the Nintendo Switch software (known as Horizon OS) to create interoperability with Nintendo Switch games and software with the Windows, macOS, and GNU/Linux operating systems.

Any aggressive DMCA claims or takedown notices against projects that explicitly disclaim piracy support, require user-provided keys, and limit functionality to interoperability (such as suyu) could constitute overreach or misuse of the DMCA.

As derived from §512(f), if Nintendo (or an affiliated entity) knowingly materially misrepresents that a project like suyu is infringing (or circumvents TPMs) when it does not, especially if they fail to consider fair use, interoperability exemptions under §1201(f), or the fact that the emulator requires user-provided keys and does not itself contain proprietary Nintendo code, they can be made liable for any Damages against suyu.

See [Legal.MD](https://github.com/suyu-emu/LEGAL.MD).

## Nextendo Network (experimental)

This fork includes an opt-in Nextendo DNS redirection layer. It does not ship Nintendo
keys, firmware, games, or account credentials.

Enable it in the emulator configuration with:

```ini
[Network]
enable_nextendo=true
nextendo_server_ip=<Nextendo server IPv4>
nextendo_nat_ip=<Nextendo NAT-check IPv4>
```

The same values can be supplied by the existing configuration system. Leave
`enable_nextendo` disabled to keep normal suyu networking behaviour.

Nextendo online support is being integrated incrementally; DNS redirection is the first
layer. Authentication, TLS/SNI handling, NAT/P2P integration, and game-specific
compatibility still require additional work.

## Building

The release workflow builds Windows x86-64, Linux x86-64, macOS arm64 and Android
arm64, plus their libretro cores. The examples below describe source builds;
release packaging and its checks are defined in the [release workflow](.github/workflows/release.yml).
Nothing here fetches a game, keys or firmware. Those are yours to supply.

Start with the maintained repository:

```sh
git clone --recursive --branch mk8-recomp https://github.com/suyu-emu/suyu-main.git suyu
cd suyu
```

Use `--branch v14.1.0` instead to build the released source. The default branch
can contain later development changes.

CMake **3.31 or newer** is required. `CMakeModules/CPMUtil.cmake` demands it and
Ubuntu 24.04 ships 3.28, so on most distributions it has to come from Kitware
rather than from the package manager.

### Linux

```sh
sudo apt-get install -y \
  build-essential git curl ca-certificates pkg-config ninja-build nasm autoconf \
  qt6-base-dev qt6-base-private-dev libqt6svg6-dev libqt6charts6-dev \
  qt6-multimedia-dev libqt6opengl6-dev glslang-tools \
  libboost-dev libboost-filesystem-dev libboost-context-dev \
  libusb-1.0-0-dev libssl-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
  libswscale-dev libswresample-dev \
  libzstd-dev liblz4-dev libgl1-mesa-dev libasound2-dev libpulse-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
  libxkbcommon-dev libxss-dev libxtst-dev \
  libwayland-dev libwayland-egl1 wayland-protocols libdecor-0-dev \
  libegl-dev libdrm-dev libgbm-dev libvulkan-dev
```

Three of those are easy to miss and each stops the build outright:
`glslang-tools` provides `glslangValidator`, which the host shader step looks up
by name; the X11 and Wayland headers are what SDL3 refuses to configure without;
and `libavfilter-dev` is required by `FindFFmpeg` even though the emulator only
decodes.

If the distribution's CMake is older than 3.31:

```sh
V=3.31.6
curl -fsSL -o /tmp/cmake.tar.gz "https://github.com/Kitware/CMake/releases/download/v${V}/cmake-${V}-linux-x86_64.tar.gz"
sudo mkdir -p /opt/cmake && sudo tar xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1
export PATH=/opt/cmake/bin:$PATH
```

Then:

```sh
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF \
  -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF \
  -Dfmt_FORCE_BUNDLED=ON
cmake --build build --target suyu suyu-cmd
```

The package metadata requires fmt 10 or newer because `logging.h` uses
`format_string::get()`. A distribution shipping fmt 9 selects the pinned
bundled release automatically. The explicit flag above also works and keeps
the dependency choice fixed.

Binaries land in `build/bin`.

### Windows

Visual Studio 2022 with the **Desktop development with C++** workload (release CI
uses MSVC toolset 14.44), Qt 6.9.3
for MSVC 2022, and the Vulkan SDK, which supplies `glslangValidator`. Qt via
aqtinstall if you do not have it:

```
aqt install-qt windows desktop 6.9.3 win64_msvc2022_64 -m qtcharts qtmultimedia
```

From a **Developer Command Prompt for VS 2022**:

```bat
cmake -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF ^
  -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF ^
  -DSUYU_EXPORT_BUILD_KIT=ON ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64"
cmake --build build --target suyu suyu-cmd export-build-kit
```

Point `CMAKE_PREFIX_PATH` at wherever Qt actually is; forward slashes save a
quoting argument with CMake. glslang 16 renamed `glslangValidator` to `glslang`
and suyu's CMake still searches for the old name, so if configure stops with
*"Required program `glslangValidator` not found"*, add
`-DGLSLANGVALIDATOR="C:/path/to/glslang.exe"`.

`suyu.exe` needs the Qt runtime beside it to start — `windeployqt` on the built
executable copies it in.

Keep `build/bin/export-build-kit` beside the application to support Windows
Static/Hybrid Build exports without a full source checkout. The kit uses MSVC
Release objects and requires an x64 MSVC toolchain at least as recent as its
recorded producer version. See the [export build kit guide](tools/export_build_kit/README.md)
and [Export Game guide](docs/user/GameExport.md). Installing LLVM/Clang can improve
generated game-code compilation; the exporter reports which compiler it used.

### macOS

Apple Silicon (arm64), with the Xcode command line tools and Homebrew:

```sh
brew install cmake ninja pkgconf boost ffmpeg sdl3 libusb enet glslang nasm qt
```

Then the same configure as Linux, plus Homebrew's prefix so Qt, FFmpeg and SDL3
are found:

```sh
cmake -B build-macos -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF \
  -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF \
  -Dfmt_FORCE_BUNDLED=ON \
  -DVulkanHeaders_FORCE_BUNDLED=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build-macos --target suyu suyu-cmd
```

`-DVulkanHeaders_FORCE_BUNDLED=ON` is the macOS counterpart of Linux's
`fmt_FORCE_BUNDLED`. Homebrew's `vulkan-headers` is found while
`vulkan-utility-libraries` is not, and `AddDependentPackages` refuses that
mixture, so configure stops with *"Partial dependency installation detected"*
rather than pairing a system copy of one with a bundled copy of the other. On a
machine with neither installed the flag is not needed.

glslang 16 ships `glslang` with `glslangValidator` as a symlink to it, so the
host shader step still finds the program by the old name.

Binaries land in `build-macos/bin`: `suyu.app` and `suyu-cmd`.

MoltenVK comes from the bundled CPM package (`V380-Ori/Ryujinx.MoltenVK`,
`v1.4.1-ryujinx`), is copied into `suyu.app/Contents/Frameworks/`, and is the
copy that gets loaded: `Vulkan::OpenLibrary` tries the bundle's
`libvulkan.1.dylib` and `libMoltenVK.dylib` before anything on the loader's
search path. The app therefore does not need MoltenVK installed. Pass
`-DYUZU_USE_BUNDLED_MOLTENVK=OFF` to prefer an installed MoltenVK instead.

macOS does not have NCE support yet. `HAS_NCE` is enabled for Android and Linux
arm64 only, so the CPU runs on dynarmic's arm64 backend, whose Mach
exception handler builds and links here.

### Android

The arm64 APK and libretro core build successfully in release CI. Real-device
gameplay has not yet been qualified. The current CI setup uses Java 17, Android
SDK platform 36, NDK `28.2.13676358`, and Android SDK CMake `3.31.6`.

On a Linux build host with the Android SDK installed, `ANDROID_HOME` set, and
`sdkmanager` on `PATH`:

```sh
sdkmanager --licenses
sdkmanager "platforms;android-36" "ndk;28.2.13676358" "cmake;3.31.6"
sudo apt-get install -y ninja-build glslang-tools nasm
sh .ci/android/build.sh -t standard -b Release
```

The APK is written to `artifacts/app-mainline-release.apk`. The script uses the
configured release keystore when supplied and otherwise the repository's test
key. For the exact signing, packaging and core-build steps, use the
[Android release job](.github/workflows/release.yml).

### libretro and experimental targets

Desktop cores use `-DSUYU_BUILD_LIBRETRO_CORE=ON` and the `suyu_libretro` target.
Android additionally uses `-DSUYU_ANDROID_LIBRETRO=ON` with the NDK toolchain.
The current core video path is Vulkan; see the [libretro guide](src/libretro_core/README.md)
for core options and platform-specific configuration.

`-DSUYU_NO_JIT=ON` builds a host without Dynarmic and requires compiled game-code
coverage. iOS requires this mode and has opt-in experimental probes, rather than
a released application. FreeBSD and Linux arm64 need their own platform build
and runtime qualification; the release workflow does not currently produce
packages for them.

## License

GPL-3.0-or-later. See [LICENSE.txt](LICENSE.txt).
