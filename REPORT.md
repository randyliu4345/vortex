# CS254A Lab 2 Report: `vx.ldm` / `vx.stm`

## 1) Summary

Implemented the Matrix-Tile Load/Store extension (`vx.ldm` / `vx.stm`) in both SimX and RTL LSU paths using macro-to-uop expansion and a per-lane AGU. Verified SimX on 32/64/128 and rtlsim on 32/64 (both PASSED after RTL operand-mapping fix).

## 2) Files changed

- `sim/simx/decode.cpp`
- `sim/simx/execute.cpp`
- `sim/simx/func_unit.cpp`
- `hw/rtl/core/VX_decode.sv`
- `hw/rtl/core/VX_uop_sequencer.sv`
- `hw/rtl/core/VX_lsu_slice.sv`

## 3) SimX performance (before/after)

Configuration: `CONFIGS="-DEXT_TCU_ENABLE"`  
Baseline: `-DVX_LDM_STM_DISABLE -DEXT_TCU_ENABLE`  
Fast path: `-DEXT_TCU_ENABLE`

| Dims | Baseline instrs | `vx.ldm/stm` instrs | Instr delta | Baseline cycles | `vx.ldm/stm` cycles | Cycle delta |
|---|---:|---:|---:|---:|---:|---:|
| 32x32x32 | 58,788 | 48,036 | -18.29% | 41,370 | 42,481 | +2.69% |
| 64x64x64 | 401,700 | 317,732 | -20.90% | 209,407 | 202,125 | -3.48% |
| 128x128x128 | 3,020,580 | 2,357,028 | -21.97% | 1,591,151 | 1,437,363 | -9.67% |

Notes:

- Instruction-count reduction matches expected headline result (18-22%).
- Cycle gain increases with larger problem size.

## 4) rtlsim results

Configuration: `CONFIGS="-DEXT_TCU_ENABLE -DNUM_THREADS=16"`  
Runtime command used as in lab: `CONFIGS="-DNUM_THREADS=16" ... make run-rtlsim ...`

| Dims | Instrs | Cycles | Status |
|---|---:|---:|---|
| 32x32x32 | 42,062 | 41,128 | PASSED |
| 64x64x64 | 193,486 | 90,371 | PASSED |
| 128x128x128 | N/A | N/A | Not run |

## 6) Deviations / debug notes
- Deviations: None. The implementation follows the provided specification.
- Initial RTL run failed correctness due to `vx.stm` decode operand mapping mismatch.
- Fix applied in `VX_decode.sv`:
  - For `INST_LSU_MST`, map:
    - `rs1` = integer base
    - `rs2` = FP fragment source base (from instruction `rd` field / `fs`)
    - `rs3` = integer `ldm` (from instruction `rs2` field)
  - Preserve fragment base in `rd_v` for role derivation in LSU slice.
- After fix, rtlsim 32/64 pass.
