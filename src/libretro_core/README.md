# suyu libretro core

The current libretro video path supports Vulkan. Where RetroArch runs its Vulkan video driver (context negotiation v2), the frames are drawn in RetroArch's own Vulkan context and handed over as images; otherwise they are read back to host memory first, which costs a copy and a wait for the GPU on every frame. Core options are read when
content loads; close and reopen content after changing an option. The exception
is **Show Game Performance** (on by default), which applies immediately: once a
second it shows the game's own frame rate and the average frame time, or the number
of shaders being built while there are any.
RetroArch's FPS counter counts every frame it presents, including repeats while
the game has not drawn a new one, so it can read far higher. RetroArch's
**Restart** command invokes `retro_reset`, which is not implemented by this
core. Close and reopen content to restart it. Save states are unsupported.

Up to four players, one per RetroArch port, each a Pro Controller. The core
names every input with the Switch's own labels (A, B, ZL, +, ...), so Quick
Menu > Controls lists and remaps them. Player 1 is always connected; players
2-4 are connected when their port is first used (a button, or a stick pushed
well off centre), as plugging a controller in, so games do not see controllers
nobody holds. Setting a port's device to None takes its input away, and for
players 2-4 also disconnects their controller.

To load a base XCI/NSP with a separate update, set
`SUYU_LIBRETRO_UPDATE_PATH` to the update NSP/XCI before starting RetroArch and
select the **base XCI/NSP** as content. The core uses the existing read-only
explicit-update verifier. It rejects an unreadable, mismatched or ambiguous
pair before the guest starts and checks the selected ExeFS modules and RomFS.
The update is scoped to that content session and is not installed to NAND.
If the variable is unset, ordinary content loading applies.

For a previously verified deconstructed `exefs/main` plus sibling `romfs.bin`,
set `SUYU_LIBRETRO_APP_VERSION` to the numeric application version and
`SUYU_LIBRETRO_DISPLAY_VERSION` to its display text before loading. The core
requires a matching AArch64 NPDM and refuses this override when the explicit
base/update provider is active. Verify the extracted module and RomFS identities
against the intended update before using this override; it does not select or
install an update by itself.

When online play is enabled, `SUYU_LIBRETRO_ROOM_SERVER`,
`SUYU_LIBRETRO_ROOM_PORT`, and `SUYU_LIBRETRO_ROOM_NICKNAME` set the room
endpoint and nickname. Their defaults are `127.0.0.1`, `24872`, and `Player`.
The port must be a decimal integer from 1 through 65535. Set these variables
before loading content. These free-form values are not exposed as misleading
single-choice core options.

For an Android ARM64 libretro build, configure with
`SUYU_BUILD_LIBRETRO_CORE=ON`, `SUYU_ANDROID_LIBRETRO=ON`, and
`YUZU_USE_BUNDLED_OPENSSL=OFF` using the Android toolchain. This opt-in mode
omits standalone app JNI glue and builds the pinned OpenSSL source package with
`no-asm` for emulator CPU compatibility. The standalone Android app keeps its
default build path when `SUYU_ANDROID_LIBRETRO` is OFF. Android properties
`debug.suyu.libretro.update`, `debug.suyu.libretro.tas`, and
`debug.suyu.libretro.perf` can supply the corresponding update path, TAS path,
and performance switch if the environment variable is absent. Clear these
properties after testing.
