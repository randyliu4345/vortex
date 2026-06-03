# 5-Minute Presentation — Demo Script

**Total target:** ~5:00 demo + ~6:00 talk if using security slides 2–3 (~90 s extra)  
**Prereq:** Vortex built in `build/`, `DEBUG=3` simx, lab app compiled.

Rehearse once the day before; use `-n4` only.

---

## Before you start (off-stage / previous slide)

```sh
cd /path/to/vortex/build
source ci/toolchain_env.sh
../configure --xlen=32 --tooldir=$HOME/tools   # if not done
make -s
DEBUG=3 make -C runtime/simx
make -C tests/regression/cfg_diverge_lab
# Optional: pre-run demo so Graphviz PNG exists
../cfg_recovery/demo.sh tests/regression/cfg_diverge_lab
```

Have open in tabs/windows:
1. Terminal in `build/`
2. `tests/regression/cfg_diverge_lab/kernel.cpp` (editor)
3. Graphviz or pre-exported `cfg_recovery/out/demo/cfg_warp0.png`
4. `cfg_recovery/out/demo/eval.json`

---

## Minute 0:00–0:45 — Security framing (Slides 2–3)

**Say:**

> “This is a security project because GPU kernels are often untrusted or obfuscated—you can’t rely on source. Attacks and bugs show up as control flow: unexpected branches, warp divergence, bad pointers. Malware and firmware analysts recover dynamic CFGs from traces for the same reason: to see what actually executed. We built that layer for Vortex SimX, software-only, with metrics so the graph is evidence-backed—not a hand-drawn picture.”

**Gesture:** Slide 3 layer cake; optional one-liner on slide.

---

## Minute 0:45–1:15 — Title + technical problem (Slides 1, 4)

**Say:**

> “GPU programs don’t execute like a single CPU thread. Warps share fetch but can branch differently. Static disassembly misses that; our tool reads SimX debug logs and recovers a warp-aware **dynamic** CFG.”

**Gesture:** Slide 4 static vs dynamic diagram.

---

## Minute 1:15–2:15 — Architecture (Slide 5)

**Say:**

> “We stayed software-only—no RTL changes. The workflow is: run a kernel on SimX with debug enabled, capture `run.log`, parse each committed instruction with warp ID and thread mask, build path edges when PC doesn’t fall through by four bytes, group those into dynamic basic blocks, then lift to a CFG and export to Graphviz.”

> “Tools live in `cfg_recovery/`; all generated files go under `build/cfg_recovery/` so we don’t pollute the source tree.”

**Point at pipeline diagram:** parse → build → visualize → eval.

---

## Minute 2:15–3:00 — What the graph means (Slide 6)

**Say:**

> “Each node is a dynamic block— a maximal straight-line run in the trace. Red dashed edges are branches or mask changes. Diamonds in the legend are divergence and merge points we infer when a block has multiple successors or predecessors.”

> “Block IDs include the first instruction UUID so loop iterations don’t incorrectly merge.”

---

## Minute 3:00–3:30 — Validation (Slide 7)

**Say:**

> “We score the result against the same trace: ninety-five percent of instruction PCs must fall inside some block; ninety-five percent of CFG edges must be supported by a real path transition; and we require at least one divergent edge. Our lab kernel `cfg_diverge_lab` is intentionally small—nested if, loop, switch—so we can sanity-check by eye.”

**Optional one-liner:** “There’s also `cfg_diverge_obf` with opaque predicates; same behavior, messier source.”

---

## Minute 2:45–4:30 — LIVE DEMO (Slide 6)

**Switch to terminal. Speak while typing or use pre-opened history.**

### Step 1 (~30 s) — One command

```sh
cd build
../cfg_recovery/demo.sh tests/regression/cfg_diverge_lab
```

**Say while it runs:**

> “This captures the trace, parses seventeen hundred-ish events on a tiny run, builds `cfg.json`, and runs the evaluator.”

**Expected tail output:** `wrote CFG with 4 warps`, then JSON with `"passed": true`.

### Step 2 (~30 s) — Show eval

```sh
cat cfg_recovery/out/demo/eval.json
```

**Say:**

> “PC coverage one point zero, edge soundness above threshold, passed true on warp zero.”

### Step 3 (~45 s) — Connect to source

**Open `kernel.cpp`, scroll to `CFG_LAB_FLAT` nested if.**

**Say:**

> “This nested if is what creates warp-dependent paths—different `task_id` values take different adds before the loop and switch below.”

### Step 4 (~45 s) — Show graph

**Open `cfg_warp0.png` or Graphviz with `cfg_warp0.dot`. Zoom to a red dashed edge.**

**Say:**

> “Here’s warp zero’s dynamic graph. Red edges are control transfers; you can see multiple paths and merge structure without reading every line of the log.”

**If Graphviz missing:** “The pipeline still writes the DOT file; install Graphviz to export PNG.”

---

## Minute 5:15–5:45 — Close (Slide 10)

**Say:**

> “Takeaway: for security you need visibility into untrusted GPU control flow. Vortex SimX traces are enough to recover warp-aware CFGs automatically, validate them against the trace, and show obfuscation doesn’t hide behavior from dynamic analysis. The pipeline is demo-ready; taint and policy checks are the natural next layer.”

> “Docs are in `docs/cfg_recovery.md`. Questions?”

---

## Timing cheat sheet

| Segment | Target |
|---------|--------|
| Security framing (slides 2–3) | 0:45 |
| Title + technical problem | 0:30 |
| Architecture | 1:00 |
| CFG semantics | 0:45 |
| Metrics | 0:30 |
| **Live demo** | **1:45** |
| Close | 0:30 |
| **Total talk** | **~6:00** (+ demo) |

---

## If demo fails (15-second recovery)

| Failure | What to say | Fallback |
|---------|-------------|----------|
| `kernel.vxbin not found` | “Need to run from the app directory—fixed in `capture_trace.sh`.” | Show pre-run `out/demo/` |
| No `DEBUG Instr` lines | “SimX wasn’t built with debug; need `DEBUG=3 make -C runtime/simx`.” | Show saved `run.log` head |
| `eval` failed | “Thresholds on soundness; lab usually passes after UUID block fix.” | Show earlier `eval.json` |
| Graphviz missing | “DOT is the deliverable; PNG is optional.” | Open `.dot` in viewer |

---

## Optional 30-second stretch (if audience asks)

**Obfuscated kernel:**

```sh
../cfg_recovery/demo.sh tests/regression/cfg_diverge_obf
```

> “Same reference output, extra opaque conditions—the trace still exposes real branches.”

**Full diverge (larger graph):**

```sh
../cfg_recovery/capture_trace.sh tests/regression/diverge -n4
../cfg_recovery/run.sh
```

---

## Q&A prep (one-line answers)

- **vs static CFG?** Dynamic = one execution; static = all paths.  
- **Why is this security?** Untrusted GPU code; CFG = evidence of executed paths; foundation for taint/CFI.  
- **Why so many blocks?** Trace-order dynamic blocks; loops revisit PCs.  
- **Agent role?** Built parsers/CFG logic iteratively; human runs `demo.sh` and judges graphs.  
- **AES / cache project?** Separate idea; this project is CFG-only.  
- **FPGA?** Same logs if SimX-equivalent debug exists; we only tested SimX.
