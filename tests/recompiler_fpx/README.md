# FP differential harness

Run from the repository root:

```sh
python tests/recompiler_fpx/run.py [--jobs N] [--cases N] [--legs 1234] [--golden FILE] [--controls]
```

Requires Python 3, CMake 3.20+ and a C/C++20 compiler (MSVC, clang-cl, GCC or
Clang). `--cc`, `--cxx`, `--generator` and `--cflags` pick the toolchain and the
flags of the generated code, for the compiler matrix.

`fpx_gen` translates every instruction word in `words.h` (generated from
`ops.txt` by `mkwords.py`) with the current emitter into `ops_soft.c`, the
default ABI 5 text, and `ops_fpx.c`, the same words with the FPX1 fast paths
(ABI 6 feature bit 2, `recomp_runtime.h`). On an AArch64 host it also writes
`hw.c`, which executes the same words natively. `fpx_driver` then runs
identical inputs through each implementation and compares the whole of q0, x0,
NZCV and the final guest FPSR, for ten FPCR settings (0, FZ, DN, RP, RM, RZ,
FZ|DN, FZ|RP, FZ|RM, FZ|RZ) and start FPSRs with and without IXC. It also
reports the FPX1 fast-path hit rate per word (lane operations that kept the
native result, with IXC set: at FPCR 0 over L1, and at FPCR FZ over L1 and L3).

Legs:

1. L1, random: edge, raw, moderate, near-underflow, near-overflow, short-mantissa
   and near-integer operands, integer sources, forced cancellation.
2. L2, special grid: all pairs (and triples for three-operand forms) of 64
   special values per width, and 64 special integers.
3. L3, adversarial: products that put a*b+z within 2^-60 of a binary32
   midpoint, exact ties, results around the smallest normal and FLT_MAX+ulp/2,
   subnormal inputs with normal results, inf*0, tiny exact sums, sums and
   differences of normal operands at, just above and just below the smallest
   normal, and a*b+z cancelling into the subnormal range or onto the smallest
   normal (the FZ boundary). Run at FPCR 0 and at every FZ setting.
4. L4, exhaustive unary: every 2^32 input (`--l4-step 1`) of FSQRT S, FCVT S->D,
   FCVTZS/FCVTZU S->W and SCVTF/UCVTF W->S at FPCR 0 and FZ.

L5 closes the chain to hardware. On an AArch64 host, `--write-golden FILE`
records one FNV-1a hash of the hardware results per leg (1, 2, 4), word and
FPCR; any other host then checks its own results against that file with
`--golden FILE`. Inputs are seeded per word, FPCR and leg, so the file does not
depend on which words or shards are run, and the `--cases`/`--l4-step` values
must match the ones the file was written with.

On an AArch64 host `diff` compares soft and FPX1 against the hardware; on other
hosts it compares FPX1 against soft, and L5 ties soft to the hardware.

L6, `--controls`, are negative controls that must fail: `nokeep` (FPX1 with the
keep test reduced to "always", like the native build without flags) must show
FPSR and value mismatches; `nomid` (no binary32 midpoint test in the binary64
FMA emulation) must show value mismatches in L3 on x86-64 (AArch64 uses the
hardware FMA and has no such test, so it is exempt there); `nofz` (the gate open
under FPCR.FZ but every helper told FZ is clear) must show value and FPSR
mismatches; `mxcsr` runs FPX1 with the host
FP mode poisoned (x86-64: FTZ, DAZ, round toward zero; AArch64: FZ and round
toward zero) and must show mismatches; `env` poisons the mode the same way but
first lets the host's own check (`core/arm/recomp/guest_fp_env.h`, as ArmRecomp
runs it) repair it, and must show none. `inhibit` sets the host's kill-switch
bit (bit 32 of fpcr) and requires the exact body's result, no fast-path hit,
and MRS/MSR FPCR that neither expose nor clear the bit. Hosts differ in whether FPX1 double
precision FMA forms have a fast path (x86-64 needs `__FMA__`), so their hit
rate can be 0 there.
