# Mod helpers

## ultracam_totk143_fix.py

Fixes **UltraCam 3.0** (nx-optimizer commit 9781528441) crashing shortly after
loading a save on **Tears of the Kingdom 1.4.3**. UltraCam's built-in offsets stop
at 1.4.2; on 1.4.3 one of its pattern searches (CameraControl) no longer matches,
which leaves a half-applied hook. The script corrects that pattern in your own
copy of `exefs/subsdk3` (three ASCII characters of a text pattern).

Run (Python 3, standard library only, nothing is downloaded):

    python ultracam_totk143_fix.py <path to UltraCam\exefs>

- It only accepts the exact original UltraCam 3.0 `subsdk3` (SHA-256 checked); any
  other version is refused and left untouched.
- The original is saved as `subsdk3.orig` (never overwritten).
- `--check` reports status only; `--restore` puts the backup back.
- The result is verified against a known-good SHA-256 before and after writing.

UltraCam also needs its config file
`sdmc/UltraCam/TOTK/Config/maxlastbreath.ini` (normally written by NX Optimizer)
to raise the frame rate.

### License and attribution

UltraCam is by MaxLastBreath, https://github.com/MaxLastBreath/nx-optimizer,
licensed CC BY-NC 2.0 (non-commercial, attribution required). This script only
patches a copy you already have and contains none of the mod's code. Please
support the original author.
