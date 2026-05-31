# Dynamic CFG Recovery (Vortex / SimX)

Recover control-flow graphs from SimX debug traces of GPU kernels (warp-aware, with divergence annotations).

**Documentation:** [docs/cfg_recovery.md](../docs/cfg_recovery.md) — milestones, validation, and agent workflow.

## Quick start (stub)

```sh
# 1. Capture a trace (from repo root)
./ci/blackbox.sh --driver=simx --app=diverge --debug=3 --log=cfg_recovery/run.log

# 2. Run the pipeline (stubs raise NotImplementedError until implemented)
./cfg_recovery/run.sh cfg_recovery/run.log
```

## Layout

| File | Role |
|------|------|
| `parse_simx.py` | `run.log` → normalized instruction events |
| `pc_map.py` | Kernel ELF → PC → symbol map |
| `build_cfg.py` | Events → basic blocks, edges, divergence metadata |
| `visualize.py` | `cfg.json` → Graphviz `.dot` / `.png` |
| `run.sh` | End-to-end driver |
| `tests/` | Unit tests and log fixtures |
