Run from an x64 Visual Studio developer environment:

```
python -B tests/recompiler_partition/run.py --build .omx/partition-regression
```

The fixture exports two synthetic modules with FM1/GG1/FPX1 enabled, once with
the production 32 MiB budget and once with a 4096-byte budget. It compares every
generated block body and dispatch entry, validates contiguous GG1 local slots,
seen-array lengths and dispatch segment counts, then builds and runs the existing
single-module and multi-module smoke drivers against both exports. Multi-block
units must stay inside the selected byte budget. A single block exceeding that
budget is preserved alone; instruction splitting is intentionally out of scope.
An additional 20,001-block fixture verifies the original 20,000-block ceiling.
