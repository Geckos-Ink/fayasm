# ESP32 Trap + Offload Sample

This sample demonstrates how to trap selected functions and spill/load microcode plus linear memory using an SD card on ESP32-class targets without PSRAM.

What it shows:
- Optional per-function traps that call back into the host before execution.
- JIT cache spill/load hooks that persist prepared microcode to SD.
- Linear memory spill/load hooks that move WASM memory to SD on demand.

Notes:
- The JIT spill/load in `main.c` writes a versioned opcode stream (`JIT_MAGIC` + version + opcodes), then rebuilds microcode on load. This avoids persisting raw function pointers across boots.
- The sample uses `/sdcard` paths. Mount your SD card there before running.

## SD Wear / Performance / Retention Guidance

Use this sample as a baseline, then tune for your board and SD profile:

1. Spill less often:
   - Prefer spilling only cold functions or memory pages after idle windows.
   - Avoid spilling on every call/loop; debounce with a minimum interval or hit counter.
2. Reduce write amplification:
   - Keep a small in-RAM dirty map and spill only changed ranges where possible.
   - Batch writes (single larger write is usually better than many tiny writes).
3. Validate persistence:
   - Add a payload checksum (CRC32 or stronger) and reject corrupted blobs on load.
   - Keep a version field (already present for JIT blobs) and migrate or invalidate old records safely.
4. Plan retention behavior:
   - Decide upfront if offload is a cache (rebuild on miss) or durable state (must survive reboot).
   - For durable state, use atomic file replacement (write temp + fsync + rename).
5. Avoid SD hot spots:
   - Rotate file names or use a ring of slots for high-frequency updates.
   - Prefer wear-leveled FS options when available in ESP-IDF.
6. Measure before locking policy:
   - Track average spill/load latency, p95 latency, and bytes written per minute.
   - Add load-failure counters and fallback paths (re-prepare JIT / zero-fill memory).

Build/Run (ESP-IDF style):
1. Enable the ESP32 compile-time target in CMake: `-DFAYASM_TARGET_ESP32=ON`
2. Ensure your SD card is mounted at `/sdcard`.
3. Copy a module to `/sdcard/app.wasm` (or define `FAYASM_SAMPLE_WASM_PATH`).
4. Build and flash your firmware as usual.

Runtime behavior:
- The sample forces prescan and microcode readiness by loosening the JIT thresholds.
- It spills memory index 0 right after module attach, so the first memory access reloads from SD.
- It traps function index 0 and attempts to reload JIT microcode from SD on entry.

If you need a different function index, update `trap_state.target_function` in `main.c`.
