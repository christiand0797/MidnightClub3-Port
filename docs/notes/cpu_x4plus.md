# Bounded CPU x4plus loading-art backend — 2026-10-24

`tools/texture_pack/cpu_x4plus.py` provides an isolated candidate backend while a human uses the GPU. It does not
start Vulkan/CUDA, publish active replacements, alter original assets or reuse NCNN provenance. Python 3.12 has
PyTorch 2.10.0+cpu installed; no BasicSR/Real-ESRGAN package installation was needed.

The architecture implements official [BasicSR RRDBNet](https://github.com/XPixelGroup/BasicSR/blob/master/basicsr/archs/rrdbnet_arch.py)
with 3 input/output channels, 64 features, 23 RRDBs, 32 growth channels, 0.2 residual scaling and two nearest-neighbor
2× stages. Local `RealESRGAN_x4plus.pth` is loaded with `weights_only=True` and `map_location="cpu"`; all 702 state
keys, tensor shapes, float32 types and finite values are validated before strict loading. Model SHA-256:
`4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1`.

An independent check executed only the official architecture's three class definitions, removed registry decoration
and skipped initialization before replacing every parameter strictly. Official architecture source SHA-256 was
`6aa8eb3f77e918b90eeff4ebfaf8745abe0c40eb8fcc03abbc1928693d70ab58`. For a 13×15 synthetic RGB float32 input,
the official and local forward graphs produced bit-identical 52×60 outputs, maximum absolute difference 0.
This verifies that measured case with those exact weights; no NCNN output equivalence is claimed.

Both PyTorch thread pools are capped at two. Each input is bounded to 1,048,576 pixels and 1,024 tiles; tile size
is 8–96 and tile padding 0–32. The default 64/10 padded crop/stitch convention follows the
[official Real-ESRGAN helper](https://github.com/xinntao/Real-ESRGAN/blob/master/realesrgan/utils.py).
Output pixels are stitched on the CPU rather than retaining an entire float32 output tensor. An optional stop
file is checked between tiles; no partially generated output is published.

```python
import cpu_x4plus as cpu
engine = cpu.CpuX4Plus()  # Reuse serially; never create duplicate inference workers.
record = cpu.upscale_original(
    source, destination,
    original_root=network_input_directory,
    expected_source_sha256=hash_pinned_by_joined_plan,
    engine=engine, tile=64, tile_pad=10,
)
```

Inputs must be pinned PNGs beneath the supplied original-derived root. The joined garage-photo worker's plan
establishes its composite-to-original lineage; this backend verifies the exact composite bytes. Active replacements
cannot be fed back as original input. Outputs must be new PNGs in an isolated `.local` directory outside the source
root. Existing outputs remain intact, including a competing publication created during inference. A complete PNG
is saved, decoded and pixel-validated before same-directory atomic no-overwrite hard-link publication.
Original alpha, when present, is resized separately with Lanczos. Source/model/module changes abort publication.

Returned records include source/output hashes and dimensions, absolute model/module paths and hashes, architecture,
strict key validation, PyTorch version, device, precision, thread/tile settings and elapsed time. The loading-photo
worker must keep these PTH records separate from its planned NCNN command/context.

Validation: 15 Python tests pass, including real inference with CUDA device queries trapped, all parameter keys,
finite/dtype/shape rejection, fully padded tile versus whole-image pixel equality, source/model/module provenance,
alpha, bounds, interruption and publication races. A 16×16 synthetic benchmark produces 64×64 output in roughly
0.3 seconds whole / 0.6 seconds across four fully padded tiles on this machine. Proof files are
`.local/cpu-x4plus-synthetic-benchmark.json` and `.local/cpu-x4plus-official-forward-verification.json`.
No real game image was processed during backend validation; original-derived joined-photo inference and visual
comparison are separate, coordinator-owned steps.
