# CFG Recovery — Example Outputs for Slides

Copy-paste from your machine after:

```sh
cd build && ../cfg_recovery/demo.sh tests/regression/cfg_diverge_lab
```

Files live in: `build/cfg_recovery/out/demo/`

---

## Slide: “Pipeline at a glance” (numbers table)

| Artifact | Size (approx.) | Role |
|----------|----------------|------|
| `run.log` | ~11 MB | Raw SimX + driver output |
| `events.json` | ~2.2 MB | 5,977 parsed instructions |
| `warp_paths.json` | (from `run.sh`) | Per-warp PC transitions |
| `cfg.json` | ~546 KB | 4 warps, blocks + edges |
| `eval.json` | 302 B | Pass/fail metrics |
| `cfg_warp0.dot` | ~36 KB | Graphviz (export PNG for slide) |

**Warp 0 summary (cfg_diverge_lab, `-n4`):**

| Metric | Value |
|--------|-------|
| Events (warp 0) | 1,048 |
| CFG blocks (warp 0) | 239 |
| CFG edges (warp 0) | 234 |
| Divergent edges (warp 0) | 180 |
| PC coverage | 100% |
| Edge soundness | 100% |
| `eval.passed` | `true` |

---

## Slide: Validation — full `eval.json`

```json
{
  "pc_coverage": 1.0,
  "edge_soundness": 1.0,
  "divergent_edges": 180,
  "total_events": 1048,
  "total_cfg_edges": 234,
  "passed": true,
  "details": {
    "warp_id": 0,
    "thresholds": {
      "pc_coverage_min": 0.95,
      "edge_soundness_min": 0.95,
      "divergent_edge_min": 1
    }
  }
}
```

---

## Slide: Raw trace input (3–5 lines)

From `run.log` — show this is *simulator evidence*, not source code:

```
CONFIGS: num_threads=4, num_warps=4, num_cores=1, ...
DEBUG Instr: CSRRS x5, ..., cid=0, wid=0, tmask=1000, PC=0x80000000 (#0)
DEBUG Instr: AUIPC x6, ..., cid=0, wid=0, tmask=1000, PC=0x80000004 (#1)
DEBUG Instr: JAL x1, 0x17c, cid=0, wid=0, tmask=1111, PC=0x80000018 (#6)
TRACE        171: alu-unit: op=JAL, cid=0, wid=0, tmask=1111, PC=0x80000018, ... (#6)
```

**Caption:** `wid` = warp, `tmask` = active lanes, `PC` = program counter.

---

## Slide: Parsed event (one JSON record)

From `events.json`:

```json
{
  "uuid": 6,
  "core_id": 0,
  "warp_id": 0,
  "pc": "0x80000018",
  "opcode": "JAL x1",
  "tmask": "1111",
  "cycle_schedule": 131,
  "cycle_ibuffer": 139,
  "cycle_dispatch": 143,
  "cycle_commit": 149
}
```

---

## Slide: CFG block + divergent edge (warp 0)

From `cfg.json` (lab kernel region — symbol demangled in your head as `kernel_body`):

**Block (divergence + merge):**

```json
{
  "block_id": "c0w0_#329",
  "warp_id": 0,
  "start_pc": "0x80000078",
  "end_pc": "0x8000007c",
  "symbol": "_Z11kernel_bodyP12kernel_arg_t",
  "is_merge_point": true,
  "is_divergence_point": true
}
```

**Divergent edges (red in Graphviz):**

```json
{ "src_block": "c0w0_#329", "dst_block": "c0w0_#332", "weight": 1, "divergent": true }
{ "src_block": "c0w0_#331", "dst_block": "c0w0_#333", "weight": 1, "divergent": true }
```

---

## Slide: Graphviz excerpt (DOT)

First lines of `cfg_warp0.dot` — or **screenshot the rendered PNG**:

```dot
digraph cfg {
  rankdir="LR";
  subgraph cluster_c0_w0 {
    label="core 0 warp 0";
    "c0w0_#329" [label="c0w0_#329\n0x80000078 – 0x8000007c\n_Z11kernel_body...", color="red"];
    "c0w0_#329" -> "c0w0_#332" [color="red", style="dashed"];
```

**To generate PNG:** `dot -Tpng cfg_warp0.dot -o cfg_warp0.png`  
**Tip for slides:** crop to `kernel_body` nodes only (zoom in Graphviz).

---

## Slide: Source ↔ analysis (side-by-side)

**Left — `cfg_diverge_lab/kernel.cpp`:**

```cpp
// CFG_LAB_FLAT: nested if/else (warp divergence by task_id)
if (task_id > 1) {
  if (task_id > 2) value += 6;
  else value += 5;
} else {
  if (task_id > 0) value += 4;
  else value += 3;
}
```

**Right — caption under graph crop:**  
“Branches in source → divergent edges in dynamic CFG (warp 0).”

---

## Slide: Terminal one-liner (demo slide)

```text
$ ../cfg_recovery/demo.sh tests/regression/cfg_diverge_lab
wrote 5977 events to cfg_recovery/out/demo/events.json
wrote CFG with 4 warps to cfg_recovery/out/demo/cfg.json
rendered cfg_recovery/out/demo/cfg_warp0.png
"passed": true
```

---

## Slide: Obfuscated kernel (security story)

**Source (`cfg_diverge_obf/kernel.cpp`):**

```cpp
if (opaque_true(task_id) && task_id > 1) {
  if (opaque_true(task_id) && task_id > 2) value += 6;
  ...
}
```

**Caption:** Same `eval.json` thresholds pass; CFG still reports divergent edges — **behavior visible without trusting source.**

---

## Minimal fixture (if trace slide too noisy)

From `cfg_recovery/tests/fixtures/diverge_snippet.log` (12 lines, good for “how parsing works”):

```
DEBUG Instr: ADD, cid=0, wid=0, tmask=1111, PC=0x80000000 (#1)
DEBUG Instr: BR, cid=0, wid=0, tmask=0111, PC=0x80000010 (#2)
TRACE          40: pipeline-commit: cid=0, wid=0, tmask=0111, PC=0x80000010 (#2)
```
