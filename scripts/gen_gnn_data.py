#!/usr/bin/env python3
"""
Generate a temporally oversubscribed synthetic graph for SimX / Vortex
core-affinity experiments.

Topology (defaults tuned for cross-mini L1/L2 reuse):
  - 2048 nodes, 16 floats per node
  - 128 mini-partitions × 16 consecutive nodes each (smaller tiles → more
    boundary traffic per byte of compute)
  - 4 physical cores; 32 minis per core

Edges (undirected; emitted as symmetric directed CSR):
  - Tier 1 — same mini-partition:       p_tier1 (default 0.22)
  - Tier 2 — same core, different mini: p_tier2 (default 0.18)
  - Tier 3 — different cores:          p_tier3 (default 0.004)
  - Hot boundary band: with high probability, connect the last `boundary_tail`
    nodes of mini m to the first `boundary_head` nodes of mini (m+1) mod K,
    forcing repeated neighbor feature reads across the mini sequence.
  - Global hubs: each node u connects to hub (u % num_hubs) with probability
    p_hub_star so a small set of feature vectors is referenced from everywhere.

Outputs gnn_data.h with CSR, features, and mini-partition metadata.
"""

from __future__ import annotations

import argparse
import os
from typing import Set, TextIO, Tuple


# Defaults: 128 × 16 = 2048 nodes (edit together)
NUM_NODES = 2048
FEATURE_DIM = 16
NUM_MINI_PARTITIONS = 128
NODES_PER_MINI_PARTITION = 16
NUM_PHYSICAL_CORES = 4
MINI_PER_CORE = NUM_MINI_PARTITIONS // NUM_PHYSICAL_CORES


def _csr_from_edge_set(
    n: int,
    pairs: Set[Tuple[int, int]],
    rng,
) -> Tuple:
    import numpy as np

    if not pairs:
        row_ptr = np.zeros(n + 1, dtype=np.uint32)
        feats = rng.standard_normal((n, FEATURE_DIM), dtype=np.float32)
        return row_ptr, np.array([], dtype=np.uint32), feats.ravel()

    arr = np.array(sorted(pairs), dtype=np.int32)
    ui = arr[:, 0]
    uj = arr[:, 1]
    rows = np.concatenate([ui, uj])
    cols = np.concatenate([uj, ui])
    order = np.lexsort((cols, rows))
    rows = rows[order]
    cols = cols[order]

    counts = np.bincount(rows, minlength=n).astype(np.uint64)
    row_ptr = np.empty(n + 1, dtype=np.uint32)
    row_ptr[0] = 0
    row_ptr[1:] = np.cumsum(counts).astype(np.uint32)
    col_ind = cols.astype(np.uint32)

    features = rng.standard_normal((n, FEATURE_DIM), dtype=np.float32)
    return row_ptr, col_ind, features.ravel()


def build_symmetric_csr(
    *,
    p_tier1: float,
    p_tier2: float,
    p_tier3: float,
    p_boundary_band: float,
    p_hub_star: float,
    boundary_head: int,
    boundary_tail: int,
    num_hubs: int,
    seed: int,
):
    import numpy as np

    rng = np.random.default_rng(seed)
    n = NUM_NODES
    assert n == NUM_MINI_PARTITIONS * NODES_PER_MINI_PARTITION

    mini = (np.arange(n, dtype=np.int32) // NODES_PER_MINI_PARTITION).astype(
        np.int32
    )
    core = (mini // MINI_PER_CORE).astype(np.int32)

    pairs: Set[Tuple[int, int]] = set()

    def add_edge(u: int, v: int) -> None:
        if u == v:
            return
        if u > v:
            u, v = v, u
        pairs.add((int(u), int(v)))

    # --- Random tier sampling (upper triangle only) ---
    i_idx, j_idx = np.triu_indices(n, k=1)
    mi = mini[i_idx]
    mj = mini[j_idx]
    ci = core[i_idx]
    cj = core[j_idx]

    tier1 = mi == mj
    tier2 = (~tier1) & (ci == cj)
    tier3 = ci != cj

    r = rng.random(i_idx.shape[0], dtype=np.float64)
    take = (
        (tier1 & (r < p_tier1))
        | (tier2 & (r < p_tier2))
        | (tier3 & (r < p_tier3))
    )
    ui = i_idx[take]
    uj = j_idx[take]
    if ui.size:
        uu = np.minimum(ui, uj)
        vv = np.maximum(ui, uj)
        stk = np.unique(np.stack([uu, vv], axis=1), axis=0)
        for row in stk:
            pairs.add((int(row[0]), int(row[1])))

    # --- Hot boundary: consecutive minis in ring order (matches device launch) ---
    bh = max(1, min(boundary_head, NODES_PER_MINI_PARTITION))
    bt = max(1, min(boundary_tail, NODES_PER_MINI_PARTITION))
    for m in range(NUM_MINI_PARTITIONS):
        start_m = m * NODES_PER_MINI_PARTITION
        end_m = start_m + NODES_PER_MINI_PARTITION
        m_next = (m + 1) % NUM_MINI_PARTITIONS
        start_next = m_next * NODES_PER_MINI_PARTITION
        for u in range(end_m - bt, end_m):
            for v in range(start_next, start_next + bh):
                if rng.random() < p_boundary_band:
                    add_edge(u, v)

    # --- Global hubs: shared feature rows touched from many minis ---
    nh = max(1, min(num_hubs, n))
    for u in range(n):
        h = u % nh
        if rng.random() < p_hub_star:
            add_edge(u, h)

    row_ptr, col_ind, feats = _csr_from_edge_set(n, pairs, rng)

    starts = (
        np.arange(NUM_MINI_PARTITIONS, dtype=np.uint32) * NODES_PER_MINI_PARTITION
    )
    ends = starts + NODES_PER_MINI_PARTITION
    core_aff = (
        (np.arange(NUM_MINI_PARTITIONS, dtype=np.uint32) // MINI_PER_CORE).astype(
            np.uint32
        )
    )

    return row_ptr, col_ind, feats, starts, ends, core_aff


def _emit_u32_array(
    fp: TextIO,
    name: str,
    arr,
    values_per_line: int = 12,
) -> None:
    import numpy as np

    a = np.asarray(arr, dtype=np.uint32).ravel()
    n = a.size
    fp.write(f"static const uint32_t {name}[{n}] = {{\n")
    for i in range(0, n, values_per_line):
        chunk = a[i : i + values_per_line]
        line = ", ".join(f"{int(x)}U" for x in chunk)
        trail = "," if i + values_per_line < n else ""
        fp.write(f"  {line}{trail}\n")
    fp.write("};\n\n")


def _emit_f32_array(
    fp: TextIO,
    name: str,
    arr,
    values_per_line: int = 8,
) -> None:
    import numpy as np

    a = np.asarray(arr, dtype=np.float32).ravel()
    n = a.size
    fp.write(f"static const float {name}[{n}] = {{\n")
    for i in range(0, n, values_per_line):
        chunk = a[i : i + values_per_line]
        parts = [f"{float(x):.8g}f" for x in chunk]
        line = ", ".join(parts)
        trail = "," if i + values_per_line < n else ""
        fp.write(f"  {line}{trail}\n")
    fp.write("};\n\n")


def write_header(
    path: str,
    row_ptr,
    col_ind,
    node_features,
    mini_partition_start_node,
    mini_partition_end_node,
    mini_partition_core_affinity,
    *,
    p_tier1: float,
    p_tier2: float,
    p_tier3: float,
    p_boundary_band: float,
    p_hub_star: float,
    boundary_head: int,
    boundary_tail: int,
    num_hubs: int,
    seed: int,
) -> None:
    import numpy as np

    num_nodes = NUM_NODES
    nnz = int(col_ind.size)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)

    with open(path, "w", encoding="utf-8") as fp:
        fp.write(
            "/* Auto-generated by scripts/gen_gnn_data.py - do not edit by hand. */\n"
        )
        fp.write(
            f"/* Graph: {num_nodes} nodes, {NUM_MINI_PARTITIONS} mini-partitions "
            f"({NODES_PER_MINI_PARTITION} nodes each), {NUM_PHYSICAL_CORES} cores "
            f"({MINI_PER_CORE} minis/core). Symmetric CSR nnz={nnz}. */\n"
        )
        fp.write(
            f"/* Edge tiers: t1={p_tier1}, t2={p_tier2}, t3={p_tier3}; "
            f"boundary_band p={p_boundary_band} (head={boundary_head}, tail={boundary_tail}); "
            f"hub_star p={p_hub_star} (hubs={num_hubs}); seed={seed}. */\n\n"
        )
        fp.write("#ifndef GNN_DATA_H\n#define GNN_DATA_H\n\n")
        fp.write("#include <stdint.h>\n\n")

        fp.write(f"#define GNN_NUM_NODES {num_nodes}U\n")
        fp.write(f"#define GNN_FEATURE_DIM {FEATURE_DIM}U\n")
        fp.write(f"#define GNN_NUM_MINI_PARTITIONS {NUM_MINI_PARTITIONS}U\n")
        fp.write(f"#define GNN_NODES_PER_MINI_PARTITION {NODES_PER_MINI_PARTITION}U\n")
        fp.write(f"#define GNN_NUM_PHYSICAL_CORES {NUM_PHYSICAL_CORES}U\n")
        fp.write(f"#define GNN_MINI_PARTITIONS_PER_CORE {MINI_PER_CORE}U\n")
        fp.write(f"#define GNN_CSR_NNZ {nnz}U\n\n")

        fp.write("/* CSR row pointers (length GNN_NUM_NODES + 1). */\n")
        _emit_u32_array(fp, "row_ptr", row_ptr)
        fp.write("/* CSR column indices (length GNN_CSR_NNZ). */\n")
        _emit_u32_array(fp, "col_ind", col_ind)
        fp.write(
            "/* Flattened node features: node major, "
            "GNN_NUM_NODES * GNN_FEATURE_DIM floats. */\n"
        )
        _emit_f32_array(fp, "node_features", node_features)

        fp.write(
            "/* Half-open node id range [start, end) for each mini-partition. */\n"
        )
        _emit_u32_array(fp, "mini_partition_start_node", mini_partition_start_node)
        _emit_u32_array(fp, "mini_partition_end_node", mini_partition_end_node)
        fp.write(
            "/* mini_partition_core_affinity[k] = physical core for mini k. */\n"
        )
        _emit_u32_array(fp, "mini_partition_core_affinity", mini_partition_core_affinity)

        fp.write("#endif /* GNN_DATA_H */\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        default="gnn_data.h",
        help="Output C header path (default: ./gnn_data.h)",
    )
    parser.add_argument("--seed", type=int, default=42, help="RNG seed (default: 42)")
    parser.add_argument(
        "--p-tier1",
        type=float,
        default=0.22,
        dest="p_tier1",
        help="Intra mini-partition edge probability (default: 0.22)",
    )
    parser.add_argument(
        "--p-tier2",
        type=float,
        default=0.18,
        dest="p_tier2",
        help="Same core, different mini-partition edge probability (default: 0.18)",
    )
    parser.add_argument(
        "--p-tier3",
        type=float,
        default=0.004,
        dest="p_tier3",
        help="Cross-core edge probability (default: 0.004)",
    )
    parser.add_argument(
        "--p-boundary-band",
        type=float,
        default=0.92,
        dest="p_boundary_band",
        help="Hot strip: last/first nodes between consecutive minis (default: 0.92)",
    )
    parser.add_argument(
        "--p-hub-star",
        type=float,
        default=0.28,
        dest="p_hub_star",
        help="Each node connects to hub (u %% num_hubs) with this prob (default: 0.28)",
    )
    parser.add_argument(
        "--boundary-head",
        type=int,
        default=4,
        dest="boundary_head",
        help="First N nodes of mini m+1 in hot boundary (default: 4)",
    )
    parser.add_argument(
        "--boundary-tail",
        type=int,
        default=4,
        dest="boundary_tail",
        help="Last N nodes of mini m in hot boundary (default: 4)",
    )
    parser.add_argument(
        "--num-hubs",
        type=int,
        default=24,
        dest="num_hubs",
        help="Number of global hub nodes 0..num_hubs-1 (default: 24)",
    )
    args = parser.parse_args()

    row_ptr, col_ind, feats, starts, ends, core_aff = build_symmetric_csr(
        p_tier1=args.p_tier1,
        p_tier2=args.p_tier2,
        p_tier3=args.p_tier3,
        p_boundary_band=args.p_boundary_band,
        p_hub_star=args.p_hub_star,
        boundary_head=args.boundary_head,
        boundary_tail=args.boundary_tail,
        num_hubs=args.num_hubs,
        seed=args.seed,
    )

    write_header(
        args.output,
        row_ptr,
        col_ind,
        feats,
        starts,
        ends,
        core_aff,
        p_tier1=args.p_tier1,
        p_tier2=args.p_tier2,
        p_tier3=args.p_tier3,
        p_boundary_band=args.p_boundary_band,
        p_hub_star=args.p_hub_star,
        boundary_head=args.boundary_head,
        boundary_tail=args.boundary_tail,
        num_hubs=args.num_hubs,
        seed=args.seed,
    )
    print(
        f"Wrote {args.output} (nodes={NUM_NODES}, mini_parts={NUM_MINI_PARTITIONS}, "
        f"nnz={col_ind.size})"
    )


if __name__ == "__main__":
    main()
