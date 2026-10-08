# Publish HID callbacks after constructing their dependencies

A strict-static TOTK packed-pilot startup crashed before profiling, about
three seconds after process creation. The minidump records a read at address
`0x8`, `RCX=0`, and instruction `movzx eax, byte ptr [rcx+8]`. The direct caller
is `SevenSixAxis::OnUpdate`; the motion callback loads the controller from
`ResourceManager` and invokes it without a null check.

`InitializeHidCommonSampler` published four CoreTiming events before
`InitializeConsoleSixAxisSampler` constructed `seven_six_axis` and
`console_six_axis`. CoreTiming dispatch runs on another thread. The dump
establishes the null receiver; the source ordering is the matching race.

The fix moves all four event publications to the end of `Initialize`, after
every sampler is constructed/configured. Periods, callbacks and teardown
events remain the same. CoreTiming's scheduling/dispatch lock supplies the
publication synchronization. Adding a null guard only to the crashed
accessor would leave partially initialized callbacks observable.

The source-driven regression follows the real initializer calls, controller
allocations and callback dereferences, treating publication as an immediate
callback. On the old source it fails because both console motion controllers
are absent. On the fixed source all four publication checks pass. It guards
this ordering contract, not arbitrary transitive configuration or concurrent
duplicate calls to `Initialize`.

Validation: MSVC 14.50 app/kit build, smoke, 201,872,960 FP differential cases
(zero value/FPSR mismatches, controls active), 20,000,768 fastmem operations
across three legs, and exporter integration 111/111 pass. Independent source
review found no actionable defect. Repeated real-game startup and the packed
performance series are still to be rerun with both arms linked against the
same corrected host kit.

Private evidence: `G:/sxdb/crash-proof`, `G:/sxdb/pilot/packed1-startup-36936.dmp`,
`G:/sxdb/hid-before.log`, `G:/sxdb/hid-after.log`, and `G:/sxdb/hid-integration.log`.
The crash is not evidence of a packed FP arithmetic error; no game performance
acceptance is inferred from the fix.
