# FM1 differential test

Run from the repository root:

```sh
python tests/recompiler_fastmem/run.py [--sanitize] [--operations N] [--seed S]
```

Requires Python 3, CMake 3.20+ and a C/C++20 compiler (MSVC, clang-cl, GCC or
Clang). `--sanitize` adds AddressSanitizer and UndefinedBehaviorSanitizer on
GCC and Clang. `--generator`, `--cc` and `--cxx` pick the toolchain.

`fmdiff_gen` writes the ABI 6 (FM1) runtime and the ABI 5 runtime from the
current emitter, and a header that renames every function of the ABI 5 one to
`abi5_*`, so both link into one binary. `fmdiff` then runs random sequences of
8/16/32/64-bit loads and stores and 32/64-bit pair loads and stores (the
64-bit pair is also LDR/STR Q) in three legs on identical memory:

1. the ABI 6 helpers with `fm_limit` set, as the host enables it;
2. the ABI 6 helpers with `fm_limit` 0, which exercises `recomp_*_slow` only;
3. the ABI 5 helpers.

Every return value, the final guest memory and the full host callback log
(every call in order, with address, size and value) must be identical.

The 64 guest pages are scattered through host memory, each followed by an
inaccessible page, so a read or write past a guest page faults. A fifth of the
pages are unmapped, debug or GPU-tracked. The address-space limit varies per
iteration and is often not page aligned. Addresses favour the last 16 bytes of
a page, odd offsets, the limit, tagged or high bits, and wrap-around near 2^64.
The default is 20,000,000 operations per leg.
