# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is Scarab?

Scarab is a cycle accurate simulator for state-of-the-art, high performance, multicore CPU. Scarab's goal is to be highly accurate, while also being fast and easy to work with.

### Scarab uArchitecture:
* All typical pipeline stages and out-of-order structures (Fetch, Decode, Rename, Retire, ROB, R/S, and more...)
* Multicore 
* Wrong path simulation
* Cache Hierarchy (Private L1, Private MLC, Private/Shared LLC)
* Ramulator Memory Simulator (DDR3/4, LPDDR3/4, GDDR5, HBM, WideIO/2, and more...)  
* Interface to McPat and CACTI for system level power/energy modeling
* Support for DVFS
* Latest Branch Predictors and Data Prefetchers (TAGE-SC-L, Stride, Stream, 2dc, GHB, Markov, and more...)
* The detailed architecture of scarab simulator is described in `/home/lee/scarab/src/research_scarab_src.md`
* Also the detailed subsystem of branch prediction of scarab is described in `/home/lee/scarab/src/bp/research_scarab_bp.md`

## Active Project: Implement the TEA architecture on top of the Scarab simulator

The primary active research feature is **TEA (Timely, Efficient, and Accurate)** — a branch precomputation mechanism that identifies hard-to-predict (H2P) branches, traces their dependence chains into a "Block Cache", and runs a speculative TEA thread that shares the OoO backend with the main thread to issue early misprediction flushes.

## TEA Paper Reference

**Before working on TEA code**, read the original paper (PDF with figures):
`docs/TEA_info/TEA_paper_origin.pdf` (MICRO 2024, UT Austin — Deshmukh, Cai, Patt)

Key sections and their Scarab mapping:

| Paper Section | Topic | Scarab Implementation |
|---------------|-------|----------------------|
| §III-A, Fig.1-2 | Backward Dataflow Walk + Fill Buffer | `dependency_chain_cache.c`, `fill_buffer.c` |
| §III-B, Fig.1(c) | TEA Fetch via Shadow FTQ + Block Cache | `tea_fetch_stage.c` (simplified: direct dep chain lookup) |
| §III-E, Fig.3 | Multi-path bitmask OR (Block Cache) | `dependency_chain_cache.c:194-214` |
| §IV-A, Fig.4 | **Hardware architecture overview** | Master plan `src/tea/TEA_implementation_plan.md` §1 |
| §IV-B | H2P Branch Table (HBT) | `bp/hbt.h/c` |
| §IV-C | Block Cache + Fill Buffer structures | `dependency_chain_cache.h`, `fill_buffer.h` |
| §IV-D | TEA frontend (Fetch + Shadow RAT + Rename) | `tea_fetch_stage.c`, `tea_rename.c` |
| §IV-E | TEA backend (shared RS/EU, 192 PR/RS reserved) | `node_stage.h`, `exec_ports.c`, `node_issue_queue.cc` |
| §IV-F | Early misprediction flush mechanism | `exec_stage.c:554-627`, `cmp_model.c:389` |
| §IV-G | Poison bit (not implemented — oracle used instead) | — |
| Table I-II | Core + TEA parameters | `core.param.def`, `PARAMS.golden_cove` |

**Fig.4 is the most important figure** — it shows how TEA Thread (Block Cache → Fetch → Rename → Shadow RAT) connects to the Main Thread, sharing the OoO backend at the Issue stage.

## TEA Implementation Tracking

TEA documentation lives in `src/tea/`:

| Document | Purpose |
|----------|---------|
| `src/tea/TEA_implementation_plan.md` | **Master document** — architecture overview, implementation priority, known bugs |
| `src/tea/TEA_implementation_plan/` | Per-feature detailed plans (dispatch, early flush, multi-H2P, etc.) |
| `src/tea/TEA_implementation_status/` | Per-feature current status with code references and bug history |

**Before working on any TEA-related code**, read the master plan (`src/tea/TEA_implementation_plan.md`) §2 (current state) and §3 (priority), then the relevant plan/status files for the specific feature. The master plan §11 lists all per-feature plan/status docs.

**After completing a piece of TEA logic**, update the corresponding status file in `src/tea/TEA_implementation_status/` and the master plan §2/§3 if applicable.

### Current TEA State (critical context)

TEA is **not yet functional end-to-end**. Key gaps (in priority order):
1. ~~**Work A**: BW Walk Trigger~~ — ✅ 구현 완료 (`fill_buffer.c`, `core.param.def`)
2. ~~**Work G**: TEA dependency wakeup missing~~ — ✅ 구현 완료 (`tea_rename.h/c`: Shadow RAT producer 추적 + `add_to_wake_up_lists()`)
3. ~~**Work I**: Independent dispatch needed — TEA/Main share single dispatch stream, causing ASSERT failures~~ — ✅ 구현 완료 (`node_stage.c`: `tea_dispatch_to_rs()`, `tea_dispatch_retry()`; `node_issue_queue.cc`: TEA ops skip in dispatch, 2-pass scheduling)
4. **Work F**: Multi-H2P — only single H2P chain supported

Implementation priority: **A → G → I → C → F → B** (see master plan (`/home/lee/scarab/src/tea/TEA_implementation_plan.md`) §3)

## Build & Run via scarab-infra (primary workflow)

All simulations are built and run through **`~/scarab-infra`**, a Docker+Slurm automation tool.
The `scarab` repo at `/home/lee/scarab` is the source being compiled inside the container.

### Setup (one-time and already done)
```bash
cd ~/scarab-infra
./sci --init    # installs Docker, Miniconda, scarabinfra conda env, SSH keys
conda activate scarabinfra
```

### Typical workflow
```bash
cd ~/scarab-infra

# 1. Edit the experiment descriptor (workloads, configs, scarab_path, build mode)
#    Descriptors are JSON files in json/. The TEA descriptor is tea_dbg.json.
#    "scarab_build": "dbg" for debug, "opt" for optimized.

# 2. Build Scarab inside the Docker container
./sci --build-scarab <descriptor>   # e.g. tea_dbg

# 3. Run simulations (parallel across simpoints)
./sci --sim <descriptor>

# 4. Check status / logs
./sci --status <descriptor>

# 5. Kill / clean up
./sci --kill  <descriptor>
./sci --clean <descriptor>

# 6. Visualize collected stats
./sci --visualize <descriptor>
```

Simulation outputs land in `<root_dir>/simulations/<descriptor>/` (e.g. `/home/lee/simulations/tea_dbg/`).

### Debugging inside the container
```bash
./sci --interactive <descriptor>    # drops into a shell inside the Docker container
# Inside the container:
cd ~/simulations/<exp_name>/baseline/<workload>/<simpoint>
mkdir debug && cd debug
cp ../PARAMS.out ./PARAMS.in        # trim lines after the cut marker
gdb /scarab/src/scarab
```

### Claude skills
- `/sim_scarab_tea` — builds and runs TEA simulation (`tea_dbg` descriptor)
- `/analyze_tea_sim` — analyzes sim.log files from the latest TEA simulation run

### Parameters
Parameters are loaded from `PARAMS.in` in the run directory (copy from `src/PARAMS.golden_cove`, `src/PARAMS.sunny_cove`, etc.) and can be overridden via CLI flags passed to `scarab`.

## Architecture Overview

### Pipeline Stages (main thread)
```
Fetch (icache_stage) → Decode (decode_stage) → IDQ → UopQueue →
Map/Rename (map_rename) → Node/ROB (node_stage) → Issue (node_issue_queue) →
Execute (exec_stage) → Dcache (dcache_stage) → Retire
```

Key files: `src/cmp_model.c` is the top-level multi-core model that initializes all components and drives the per-cycle simulation loop.

### Per-Cycle Execution Order (critical for TEA timing)
```
cmp_cycle()
  ├── cmp_istreams()   ← recovery detection → cmp_recover() → recover_tea_on_flush()
  └── cmp_cores()      ← per processor:
       ├── update_dcache_stage()   ← TEA store buffer full → terminate_tea_thread()
       ├── update_exec_stage()     ← TEA early flush detection
       ├── update_node_stage()     ← dispatch, issue, retire (Main + TEA)
       ├── update_map_stage()      ← SRT checkpoint for early flush
       ├── ... (decode, icache)
       └── update_tea_thread()     ← TEA fetch → rename → state transition
```
**Key invariant**: `cmp_istreams()` (recovery/flush) runs before `cmp_cores()` (execution). Within `cmp_cores()`, stages run back-to-front (dcache before exec before node).

### TEA Thread Components (`src/tea/`)

| File | Role |
|------|------|
| `tea_thread.h/c` | TEA state machine (IDLE → FETCHING → EXECUTING), trigger/terminate |
| `tea_fetch_stage.h/c` | TEA dedicated fetch from Dependency Chain Cache (not Shadow FTQ — uses direct lookup approach) |
| `tea_rename.h/c` | Shadow RAT for TEA thread register renaming |
| `tea_store_buffer.h/c` | 16-entry store data cache for TEA thread stores |
| `tea.stat.def` | TEA statistics counters |

### TEA Supporting Structures in Core Files

- **HBT (Hard Branch Table)**: `src/bp/hbt.h/c` — 1024-entry table with 3-bit saturating counters tracking branch mispredictions; branches with counter > 1 are H2P.
- **Fill Buffer**: `src/fill_buffer.h` — records retired uops for backward dataflow walk to trace dependence chains.
- **Dependency Chain Cache**: `src/dependency_chain_cache.h` — TEA Fetch reads pre-combined full chains from here (indexed by H2P PC). Block Cache (`block_caches`) is also filled by BW Walk but currently unused by TEA Fetch.
- **Node Stage RS partitioning**: `src/node_stage.h` — `Reservation_Station` has `main_op_count`/`tea_op_count` and limits for TEA RS reservation.
- **Map/Rename SRT checkpoint**: `src/map_rename.h/c` — `reg_file_snapshot_srt()` / `reg_file_rollback_srt()` for TEA early flush Case 2 recovery. Shadow RAT itself is in `tea_rename.h/c`.

### Parameter System

Parameters are defined in `*.param.def` files (e.g., `src/core.param.def`, `src/bp/bp.param.def`). The macro `DEF_PARAM(var_name, MACRO_NAME, type, parser, default, const)` generates extern declarations and CLI parsing. TEA parameters live in `src/core.param.def`.

### Key Data Structures

- `Op` (`src/op.h`): the fundamental unit — represents a single micro-op through the pipeline
- `Stage_Data` (`src/stage_data.h`): inter-stage communication (array of Op pointers)
- `Node_Stage` (`src/node_stage.h`): ROB + Reservation Station management
- `Tea_Thread` (`src/tea/tea_thread.h`): per-core TEA state

### Statistics

Stats are defined in `*.stat.def` files and collected in `src/stat_files.def`. TEA-specific stats include `TEA_TRIGGERS`, `TEA_OPS_FETCHED`, `TEA_OPS_DISPATCHED`, `TEA_OPS_ISSUED`, `TEA_OPS_EXECUTED`, `TEA_EARLY_FLUSH`, `TEA_FETCH_CHAIN_HIT/MISS`.

## Custom Claude Skills

- `/sim_scarab_tea` — builds and runs TEA simulation via scarab-infra (`tea_dbg` descriptor)
- `/analyze_tea_sim` — analyzes sim.log files from the latest TEA simulation run, extracts errors and TEA statistics
