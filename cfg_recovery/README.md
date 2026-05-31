# Dynamic CFG Recovery (Vortex / SimX)

Recover control-flow graphs from SimX debug traces of GPU kernels (warp-aware, with divergence annotations).

**Documentation:** [docs/cfg_recovery.md](../docs/cfg_recovery.md)

## Layout

| Location | Contents |
|----------|----------|
| `cfg_recovery/` (repo) | Versioned Python tools and tests |
| `build/cfg_recovery/` | Generated logs and JSON/PNG outputs (after `../configure`) |

## Quick start

All build and trace steps run from **`build/`** (see Vortex README: `cd build && ../configure ...`).

```sh
cd build
source ci/toolchain_env.sh
make -s
DEBUG=3 make -C runtime/simx

# M0: capture trace → build/cfg_recovery/run.log
../cfg_recovery/capture_trace.sh tests/regression/diverge -n4

# M1/M2 + pipeline (from repo root or build/)
../cfg_recovery/run.sh
# or, from build/:
../cfg_recovery/run.sh cfg_recovery/run.log tests/regression/diverge/kernel.elf
```

```sh
python3 -m unittest discover -s ../cfg_recovery/tests -v

# Full project demo (after configure + make)
../cfg_recovery/demo.sh tests/regression/cfg_diverge_lab
```

Regression apps: `tests/regression/cfg_diverge_lab`, `tests/regression/cfg_diverge_obf` (re-run `../configure` in `build/` once).
