# Owned GUI shutdown regression

Matched-runtime validation on 2026-10-09 reproduced an access violation after a
successful MK8D Source export on the local pre-fix GUI. The patched GUI completed
the same export and exited 0 with no caught exception or forced cleanup. Both used
identical Qt 6.10.3 DLL hashes and a held modal RPC socket; registry fingerprints
were unchanged. Published v0.0.13 also reproduced the worker-thread access violation.
Evidence: `G:/sxdb/release14-investigation/gui-shutdown/debug/matched-verdict.md`.
This proves the tested Source/close path, not every shutdown path or game rendering.

Use an isolated release-like folder and user directory prepared by the existing
export integration setup (including ONLY its synthetic placeholder keys when Source
fixture is requested). Do not use a developer's configured emulator or real games.
`close_owned_gui.py` starts and owns the GUI process; every HWND is filtered by that
PID and visible root/owner relationship. Timeout termination is a failed check.

First run identical fixture against published13 to reproduce the pilot failure;
then patched14. For each use fresh output/work and an unused MCP port:

- No fixture: generic initial library population followed by normal main closure.
- `--fixture <synthetic NSP> --close-order modal-main`: Source success/running=false,
  modal closure immediately followed by main closure. Repeat with `--rescan-delay 2`
  to separate recently queued population from long export cleanup.
- `--fixture ... --close-order main`: main closes while completed modal remains open.
- `--fixture ... --close-order all`: reproduce pilot's all-visible-PID-window cleanup.
- Repeat with `--disconnect-fire`; otherwise pending modal RPC socket remains alive.

Require `shutdown-result.json` passed=true/exit_code0 and NO forced_cleanup for fixed
binary. Nonzero/failfast is a failure. Capture Windows fault module/stack/subcode for
baseline; code C0000409 alone cannot identify its mechanism. This helper prepares
Source code only when requested, never builds native modules or launches a game.

Additional interactive/Qt fixture checks: declined ConfirmClose must still permit
later population, as must hiding/minimizing main and rejecting only export dialog.
After accepted closure, queued directory reloads must not start a worker; repeated
ShutdownPopulate must be harmless. A deterministic Qt owner-order test can pause a
real GameListWorker before provider access, request close, then release worker and
assert join finishes before provider/system destruction. Do not replace this with a
test that merely mirrors the latch.

The implementation permanently latches population only AFTER ConfirmClose accepts,
and destructor backstop joins before C++ members die. Worker cancellation uses its
existing tryTake/CancelBeforeRun and stop/wait path. Hazard predates v13; this change
is not yet evidence for the user's game failure.

Latest baseline attempt used an extracted directory and timed out BEFORE any close;
its forced cleanup supplies no shutdown evidence. The next reference fixture is
G:/ri14/games/SynthNsp.nsp. The helper now refuses directory/non-NSP fixture inputs
before launching, gives readiness/modal/export phases separate deadlines, and records
phase, last_status, trigger_result, error and pre-cleanup exit state. An unavailable
modal during export is an immediate failure, not a busy-export timeout. Only a record
with export_complete=true followed by an actual closing phase can support a
post-export shutdown conclusion. The parent's exact PID+creation-time guard and
headless-capture environment remain intact.

Windows Source export also shows an interactive path-budget dialog above 70 output
characters. Pass --source-output explicitly when the work folder is long; the helper
checks the resolved path BEFORE starting the GUI and records source_output. Next root
baseline destination: G:/sxdb/gclose-out/v13-baseline1. Use a NEW destination for each
case so conflict dialogs do not obscure shutdown. The earlier long-output attempt
hit OutputPathMayBeTooLong before closure, not a shutdown failure.

Run against a PRIVATE PORTABLE GUI copied/prepared by the integration bootstrap,
with only synthetic fixture placeholder keys in its private user directory. APPDATA
isolation alone does not supersede an executable-adjacent portable user folder.
Never copy real keys or use the developer's configured GUI for this check.
