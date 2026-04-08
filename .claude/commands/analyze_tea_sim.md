# TEA Simulation Log Analyzer

Analyze sim.log files from the most recent TEA simulation run.

## Instructions

### Step 1: Find the most recent simulation directory

Run:
```bash
ls -t /home/lee/simulations/ | grep "tea_dbg" | head -1
```

Store the result as `<latest_dir>`. Then:

```bash
find /home/lee/simulations/<latest_dir> -name "sim.log" | sort
```

### Step 2: For each sim.log found, extract key information

For each log file, collect the following using Read tool (NOT cat):

**a) Error detection** — search for these patterns in order of priority:
- `ASSERT FAILED` — critical crash
- `Assertion.*failed` — C++ assert
- `SEGFAULT` / `Segmentation fault`
- `ERROR:` (excluding LeakSanitizer which is low-priority)
- `Scarab Done.` — indicates successful completion (no crash)

**b) For ASSERT FAILED errors**, extract:
- The exact assertion line: file path, line number, conditions (P/O/I/C values), and assertion expression
- The 3 lines after it (first stack frames with function names if available)

**c) TEA statistics** — grep for these in `sim.log`:
- `TEA_TRIGGERS`
- `TEA_OPS_FETCHED`
- `TEA_OPS_DISPATCHED`
- `TEA_OPS_ISSUED`
- `TEA_OPS_EXECUTED`
- `TEA_EARLY_FLUSH`
- `TEA_TRIGGER_SKIP_ACTIVE`
- `TEA_TRIGGER_SKIP_NO_CHAIN`
- `TEA_FETCH_CHAIN_HIT`
- `TEA_FETCH_CHAIN_MISS`

### Step 3: Organize and report results

Group results by:
1. **Config** (e.g., TEA_on_tage64k, TEA_on_gshare)
2. **Workload** (mcf, leela)
3. **Simpoint** (numeric directory)

For each log, report in this format:

```
[Config / Workload / Simpoint]
Status: CRASHED / COMPLETED
Error: <assertion file>:<line>: <condition> | <expression>
Stack: <top 2-3 function names>
TEA Stats:
  TRIGGERS: X  |  FETCHED: X  |  DISPATCHED: X  |  EXECUTED: X  |  EARLY_FLUSH: X
  SKIP_ACTIVE: X  |  SKIP_NO_CHAIN: X  |  CHAIN_HIT: X  |  CHAIN_MISS: X
```

### Step 4: Cross-workload comparison

After individual reports, summarize:

- **Are errors consistent** across simpoints of the same workload?
- **mcf vs leela differences**: Do they crash at different locations? Different TEA stats?
- **Config differences**: TEA_on_tage64k vs TEA_on_gshare behavior?
- **Root cause hypothesis**: Based on error location + TEA stats, what is the most likely cause?

Focus the root cause analysis on TEA-related code paths (thread_id==1 checks, shadow RAT, node_stage TEA dispatch, etc.) given the current implementation state.
