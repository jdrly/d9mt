# Development notes

Working notes from d9mt-v2's development, kept for context. They are not
maintained and may describe plans, paths or scripts that have since changed.
The top-level [README](../../README.md) is the current documentation.

| File | What it is |
|---|---|
| `SOURCE_OF_TRUTH.md` | neo773's architecture plan and rules for the Metal 4 rewrite (Metal 4 only, AIR emitter, bridge policy), with build/debug notes |
| `PERF_ROADMAP.md`, `METAL4_PERF_RESEARCH.md` | neo773's performance investigation and Metal 4 CPU-overhead research |
| `WINDOW-STATE-MATRIX.md`, `WINDOWING-METAL-AUDIT.md` | neo773's windowing/presentation audit (fullscreen, occlusion, visibility-aware present) |
| `kk-audit.md` | Comparison of the backend with Mesa's KosmicKrisp Vulkan-on-Metal-4 driver (design study) |
| `SA2-LAUNCH.md` | Bring-up notes for launching a Steam title through the driver under CrossOver |

`driver/EXTRACTION-LOG.md` documents how the D3D9 frontend was extracted from
DXVK and adapted; it stays next to the code it describes.
