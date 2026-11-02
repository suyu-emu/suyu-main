# Synthetic export integration fixture

`make_fixture.py` generates a tiny, bootable, extracted-ExeFS style fixture for
suyu (stdlib Python only). Nothing is committed as a binary: every byte is
produced by the script. It contains no Nintendo keys, firmware, code or game
data, and is licensed GPL-3.0-or-later like the rest of the project.

Files written: `main` (uncompressed NSO0, flags 0), `main.npdm` (META/ACID/ACI0,
64-bit, 39-bit address space, SVC masks for 0x07/0x0B/0x27), `romfs.bin`
(level-3 RomFS holding `fixture.txt`: "suyu synthetic export fixture").

Title/program ID: `0x0100000000E57A00` (SYNTHETIC; not a real title).

Program behaviour: `svcOutputDebugString("SUYU_EXPORT_FIXTURE_OK\n")`,
`svcSleepThread(100 ms)`, then `svcExitProcess`. suyu buffers debug strings in a
flusher thread and logs them as `Debug.Emulated <Info>` (keep the sleep under
250 ms: a longer sleep let the flusher log mid-run and the process then hung in
testing). With the default logger the marker line appears in `suyu_log.txt`
right after `Process N exiting`; the log file is only flushed reliably on a
clean exit.

## Run

    python make_fixture.py OUTDIR
    # PowerShell, with an isolated profile (never use your real one):
    $env:APPDATA = "<scratch>\appdata"; $env:LOCALAPPDATA = "<scratch>\local"
    $env:SUYU_CMD_CAPTURE_HEADLESS = "1"
    suyu-cmd.exe -g OUTDIR\main

suyu-cmd currently needs `<APPDATA>\suyu\keys\prod.keys` to exist and to hold a
non-zero `header_key` (an all-zero key crashes NCA identification with a divide
by zero). For this synthetic input a made-up, obviously fake `header_key` line is
enough; no real keys are needed. Check `<APPDATA>\suyu\log\suyu_log.txt` for
`SUYU_EXPORT_FIXTURE_OK`.
