# Serial Prepared Execution Benchmarks (2026-10-31)

The previous change specialized single operators but still returned to the runtime loop for each opcode. This implementation runs a linked series of precompiled C delegates with shared temporary registers. It also removes repeated physical RAM/CPU probing from the statistics tick. These are separate changes and are measured separately below.

## Method and controls

- Baseline: committed `bd7b632` (the previous scalar-bytecode/fixed-register implementation).
- Apple M1 Pro, 16 GiB RAM, AppleClang 21.0.0.21000401, macOS ARM64. All runners use static CMake Release (`-O3 -DNDEBUG`, C99), without LTO or fast-math.
- Unchanged Rust 1.98.1 `wasm32-unknown-unknown` fixture (`-O -C panic=abort`), SHA-256 `183619c8b6d183d1e4e6b9834b6f7cffaee03a7a3df2e729fe32c1effd801814`. Emscripten was unavailable; these figures are not mixed with the historical Emscripten/native measurements.
- `before`: previous revision. `no-sequence`: current implementation with `FAYASM_JIT_SEQUENCES=0`, retaining the cached hardware probe and current single-op path. `serial`: current default circular backend with serial execution enabled. `fixed4`: serial execution with four fixed operand slots, five immediate slots, depth cap 2,048.
- All reported columns use `FAYASM_MICROCODE=1`. Both prescan flags are off. Resource/advantage gates, the 512-source-op cap, and resident cache budgets remain active. Cold execution/binding and fallback dispatch count toward the timings.
- Median of three fresh-process wall times after one excluded warmup per workload/variant. Order rotates between repetitions; parsing, startup, attach, preparation, and execution are included. No startup subtraction. No builds overlap the final run. Earlier exploratory runs are not reported.
- All results match exactly across the four runners. Large-input outputs also match the native C reference values recorded in [the prior raw data](stats_2026-10-31.json), printed to 17 significant digits. This is not a new native timing comparison.
- Reproduction tool: [benchmark_stats.py](benchmark_stats.py). Individual samples, outputs, and flags: [sequence_2026-10-31.json](sequence_2026-10-31.json).

## Results

| Workload | Before | Current, sequences off | Serial circular | Serial fixed 4 | Before / serial |
| --- | ---: | ---: | ---: | ---: | ---: |
| mean(500k) | 891.74 | 404.07 | 72.07 | 74.12 | 12.4× |
| variance(500k) | 1724.72 | 761.61 | 120.31 | 124.36 | 14.3× |
| linreg(500k) | 1663.54 | 773.15 | 124.05 | 125.67 | 13.4× |
| pearson(500k) | 2361.32 | 1109.32 | 170.54 | 176.71 | 13.8× |
| pi(500k) | 2111.51 | 972.50 | 155.71 | 157.64 | 13.6× |
| sieve(100k) | 306.28 | 166.76 | 50.26 | 51.27 | 6.1× |
| matrix(40) | 104.49 | 59.49 | 14.95 | 15.05 | 7.0× |
| pipeline(100k) | 2091.07 | 994.56 | 380.35 | 385.56 | 5.5× |
| startup | 5.91 | 5.48 | 5.19 | 5.89 | 1.1× |

Times are milliseconds; the ratio uses serial circular.

The combined change is **5.5–14.3× faster** on the eight nontrivial workloads. With probe reuse already enabled, the serial path adds **2.6–6.5×** over current single-op execution. Fixed four-slot results are close to circular; this run does not establish a general fixed-backend advantage. The pipeline remains limited by the capped prepared prefix and synchronization boundaries. Startup is dominated by process/parse noise.

The `no-sequence` control matters: attributing the entire gain to macros would be incorrect. Physical hardware probing every 64 interpreted instructions was itself a substantial bottleneck. Serial execution adds a further gain by avoiding repeated opcode/descriptor lookup, immediate reconstruction, public job-stack helpers, and return through the main runtime loop for each supported instruction.

## Execution contract

- `src/fa_ops_sequence.c` emits all 128 scalar numeric delegates (`0x45..0xC4`) and all 23 scalar memory delegates (`0x28..0x3E`) from macro lists. Constants, local get/set/tee, drop, and select also run directly. Operator, width, signedness, and conversions are fixed in the compiled delegate, with input/bounds/trap checks retained. Register arithmetic uses unsigned wrapping; `i64.rem_s(INT64_MIN,-1)` returns zero without undefined C behavior in both the sequence and generic paths.
- The first successful common decode binds argument bytes in original push order, source/next PCs, and memory index/offset. The next prepared index resolves once, then the executor follows cached links. A flat bank of 16 typed temporary values spans operations. Register pushes validate logical depth and backing capacity immediately; materializing values on an exit cannot defer an allocation failure past observable side effects.
- Up to 64 source instructions execute per batch. All source instructions, scalar-binding hits, sequence entries, and serially executed operations remain counted. The public job stack is restored at every exit, including trap and cap failure; values below a callee/control-frame base remain intact.
- Calls and control operations stay synchronization boundaries. Prefixed, table/ref/global, and other generic delegates use the already-bound context, materialize temporary registers, execute the checked handler, and end the sequence. Memory delegates use current runtime memory objects and buffers on each access; imported ownership and rebinding are preserved. Unloaded memory or failing checks enter the generic path before mutation to preserve its failure-state behavior.
- A generic delegate snapshots its complete record before invoking a handler: a memory reload callback may free/evict the active program. No program/map/record access occurs after a stopping callback. The regression test deliberately frees the program during reload under ASan.
- Opcode-only spill format version 1 is unchanged. Delegates/links/bindings are rebuilt after load; no process-local pointers are serialized. The context's hardware probe is initialized once; `fa_jit_context_update` reevaluates decisions from the snapshot and current statistics. Reinitialize a context (or explicitly replace its probe) when a fresh hardware snapshot is wanted.

## Costs and remaining limits

This is a portable C delegate executor, not native machine-code generation. Guards and an indirect call per delegated operation remain. Preparation is lazy and capped: cold instructions and instructions beyond the prepared prefix still use ordinary dispatch, which limits the combined pipeline's gain. Control boundaries and generic/prefixed families still have their existing handler cost. These limits are kept visible rather than described as full-function native compilation.

On this ARM64 ABI, prepared records grow from 56 to 120 bytes. A full 512-op chunk occupies **60 KiB**, an increase of **32 KiB** over the previous implementation. The resident cache budget includes these records; preparation also caps its source target to the byte budget. Replacement gives credit for the old resident chunk, although its temporary candidate can coexist with the old program until commit. Opcode/offset bookkeeping remains outside this existing prepared-program budget. The temporary register bank adds **384 bytes of C-stack values** per executor invocation, plus context/helper-frame overhead. `fa_Job` remains 648 bytes by default and 408 bytes in the compact profile; those sizes alone do not represent total runtime footprint.

The page-rounded Mach-O runner `__TEXT` segment grows from 196,608 to 311,296 bytes (**112 KiB more**) with the generated delegates. Circular and compact fixed runners have the same rounded segment size. These are host ABI/image measurements, not ESP32 RAM/flash estimates. Actual embedded performance, code footprint, and storage cycles require hardware validation.

Rebuild consumers: `fa_JitPreparedOp`, `fa_Runtime`, and `fa_JitConfig` gained fields. CMake propagates the fixed-backend define; slot sizes must also agree between consumers and library.

## Validation

- Full native shared/default and static compact-fixed harnesses pass with the generated Rust WASM fixtures present, including forced-prescan invocation. No missing-fixture skips.
- ASan + UBSan pass the complete harness for both circular and compact fixed storage (`detect_leaks=0`, because LeakSanitizer is unavailable on this macOS host; `halt_on_error=1`).
- Every scalar delegate is compared against descriptor execution on normal/negative/zero/overflow/NaN/infinity inputs, mismatched kinds and underflow, with sentinels across inline/spill boundaries. Memory parity covers every scalar load/store, memory 1, memory64, offsets, overflow, OOB, and wrong address kinds.
- A 255-operation chain checks register overflow/refill, source-op accounting, and batch boundaries. Additional regressions cover partial-chain materialization at the depth cap, eviction during a reload callback, probe snapshot reuse, and repeated five-immediate SIMD-lane context replay. The large-body prepare-once test now uses the minimum 64 KiB resident budget; existing spill/reload, ownership, calls, and branch tests continue to pass.

## Reproduce

From the repository root, build all variants before timing:

```sh
cmake -S . -B build/serial -DCMAKE_BUILD_TYPE=Release -DFAYASM_BUILD_SHARED=OFF
cmake --build build/serial
cmake -S . -B build/serial-fixed -DCMAKE_BUILD_TYPE=Release -DFAYASM_BUILD_SHARED=OFF -DFAYASM_FIXED_REGISTERS=ON \
  '-DCMAKE_C_FLAGS=-DFA_JOB_REGISTER_WINDOW_SIZE=4 -DFA_JOB_DATA_FLOW_WINDOW_SIZE=5 -DFA_JOB_STACK_MAX_DEPTH=2048'
cmake --build build/serial-fixed
python3 studies/runtime/benchmark_stats.py --microcode 1 \
  --variant no-sequence=build/serial/bin/fayasm_run --sequence-mode no-sequence=0 \
  --variant serial=build/serial/bin/fayasm_run \
  --variant fixed4=build/serial-fixed/bin/fayasm_run \
  --output /tmp/fayasm-sequence.json
```

For the old control, export `bd7b632` to a separate temporary source tree with `git archive`, build it using identical Release/static options, and add `--variant before=/path/to/that/build/bin/fayasm_run`. The old revision ignores the sequence toggle. All variants must use the same generated fixture via the script's `--fixture` path. `--repeats 5` lengthens the run. Omit `--microcode 1` to also collect descriptor-only controls.
