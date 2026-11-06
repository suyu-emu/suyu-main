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
  <a href="#building">Building</a> |
  <a href="#license">License</a>
</p>

---

> **This fork is under active development.** Its default branch is focused on a
> title-specialized, static-first runtime with a correctness-preserving Dynarmic
> fallback. See the [static recompiler roadmap](docs/StaticRecompiler.md) for
> what works today and what still needs to be proven.

## About

suyu is a Nintendo Switch emulator and AArch64 native recompiler written in C++. It can run decrypted Switch titles using either:

- **Ordinary emulation** — Dynarmic JIT CPU execution with suyu's GPU, audio and service emulation.
- **Static or Hybrid recompilation** — ahead-of-time compilation of AArch64 game code, using the same GPU, audio and HLE services. Hybrid permits JIT fallback; Static requires compiled coverage.

Based on [Eden](https://git.eden-emu.dev/eden-emu/eden), with suyu's own improvements to UI, recompiler, and platform support.

## Status

The current base is **v0.04**, with continued development on top. Release builds are published to
the [releases page](../../releases) by GitHub Actions when maintainers cut one.

Platforms: Windows, Linux, Android. macOS/iOS not included in this release.

## Legal Notice

suyu is a GPLv3 program, which allows fully free redistribution of its source code and releases liability of its authors for how this software is used as stated in Section 15 and 16.

The suyu Emulator program does not circumvent Nintendo's technological protection measures (TPMs) as the user is required to provide both the Nintendo Switch software & the encryption keys for these games, and the suyu Emulator uses a mode of the Advanced Encryption Standard (AES), an open encryption standard established by the US NIST, along with the encryption keys that the user themselves must lawfully acquire, to decrypt the software. As the standard is public and available to use by all, it does not constitute as the Digital Market Copyright Act's (DMCA) definition of "circumventing a technological measure" as defined in Section 1201(a)(3).

The suyu Emulator also falls under the exemptions stated in Section 1201(f) of the DMCA as this software was created for the purposes of reverse engineering the Nintendo Switch software (known as Horizon OS) to create interoperability with Nintendo Switch games and software with the Windows, macOS, and GNU/Linux operating systems.

Any aggressive DMCA claims or takedown notices against projects that explicitly disclaim piracy support, require user-provided keys, and limit functionality to interoperability (such as suyu) could constitute overreach or misuse of the DMCA.

As derived from §512(f), if Nintendo (or an affiliated entity) knowingly materially misrepresents that a project like suyu is infringing (or circumvents TPMs) when it does not, especially if they fail to consider fair use, interoperability exemptions under §1201(f), or the fact that the emulator requires user-provided keys and does not itself contain proprietary Nintendo code, they can be made liable for any Damages against suyu.

See [Legal.MD](https://github.com/suyu-emu/LEGAL.MD).

### Nintendo Switch 2

Nintendo Switch 2 emulation builds upon the same legal foundations as Nintendo Switch emulation. The suyu Emulator does not circumvent Nintendo's technological protection measures for Switch 2 software, as users are required to provide their own Switch 2 software and encryption keys, which they must lawfully acquire. The Switch 2's use of standard encryption (AES) and the requirement for user-provided keys places it under the same interoperability exemptions (DMCA Section 1201(f)) that apply to Switch 1 emulation. Additionally, Switch 2 emulation benefits from the fact that the Switch 2 platform shares significant architectural similarities with the original Switch, further reducing any potential legal exposure related to reverse engineering for interoperability purposes.

Notably, Nintendo Switch 2 game software is distributed in an unencrypted format, meaning that Switch 2 titles do not employ the same encryption-based technological protection measures found on the original Nintendo Switch. This further strengthens the legal position of Switch 2 emulation, as there can be no claims of circumventing technological protection measures when the software itself is not encrypted. Even in the case of the original Nintendo Switch, suyu has consistently maintained that requiring user-provided keys and software does not constitute circumvention under DMCA Section 1201(a)(3), a position that has been validated by the interoperability exemptions outlined in Section 1201(f).

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

### Windows

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF -GNinja
cmake --build build --target suyu suyu-cmd suyu_recomp
```

Use `--branch v14.1.0` instead to build the released source. The default branch
can contain later development changes.

CMake **3.31 or newer** is required. `CMakeModules/CPMUtil.cmake` demands it and
Ubuntu 24.04 ships 3.28, so on most distributions it has to come from Kitware
rather than from the package manager.

### Linux

```sh
sudo apt-get install ninja-build qt6-base-dev libqt6svg6-dev libusb-1.0-0-dev libssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF -GNinja
cmake --build build --target suyu suyu-cmd suyu_recomp
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
