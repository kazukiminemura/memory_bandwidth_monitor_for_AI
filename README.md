# Memory Bandwidth Monitor for AI (C++)

A real-time CLI tool that estimates memory bandwidth and shows which functions are driving it.

## What this tool does

- Real-time throughput view (windowed MB/s)
- Per-function impact table:
- Function name
- Calls in interval
- Bytes touched in interval
- Estimated MB/s contribution
- Average latency per call
- Process memory context (working set, private bytes, page-fault rate)

## Why this is useful for AI apps

Many AI workloads are memory-bound in embedding lookup, attention, and normalization paths.
This tool helps answer: "Which function increases memory traffic right now?"

## Build (Windows)

Requirements:
- CMake 3.20+
- A C++20 compiler (MSVC recommended on Windows)

Build commands:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Binary:
- `build/Release/mbm_cli.exe` (multi-config generator)
- or `build/mbm_cli` (single-config generator)

## Run

Demo mode (generates AI-like workloads and profiles them):

```powershell
./build/Release/mbm_cli.exe --demo --workers 4 --interval-ms 500
```

Run for a fixed duration:

```powershell
./build/Release/mbm_cli.exe --demo --duration-sec 10
```

Stop with `Ctrl+C`.

## Integrate into your own AI code

Include and instrument key functions:

```cpp
#include "mbm/mbm_profiler.hpp"

void attention_kernel(float* q, float* k, float* out, std::size_t n) {
	MBM_SCOPE("attention_kernel");
	// read q + k, write out
	MBM_ADD_BYTES((2ULL * n + n) * sizeof(float));
	// ... actual kernel logic ...
}
```

Notes:
- `MBM_SCOPE("name")` measures elapsed time and call count.
- `MBM_ADD_BYTES(x)` records logical bytes touched by the current scope.
- The reported MB/s is an estimate based on the bytes you annotate.

## Limitations

- This is function-level estimated bandwidth, not direct DRAM hardware counter sampling.
- Accuracy depends on correct byte annotations in hot paths.
- For CPU-uncore hardware counters, platform-specific tools (for example Intel PCM) are needed.

## Output example (simplified)

```text
window=500ms total=18432.7 MB/s ws=1420.3 MB private=1172.0 MB pf_rate=42.0/s
--------------------------------------------------------------------------------
Function                         Calls      MB/s        MB       Avg us
simulate_matmul_tiled             388     9112.4    4556.2      512.10
simulate_attention_score          1552     5278.1    2639.1      118.44
simulate_layer_norm               3104     2011.6    1005.8       33.28
```
