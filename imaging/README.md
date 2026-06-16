# PulseLink imaging (Stage 4) — frames → edge detection → BMP

The optional camera path. A device sends a grayscale frame as a series of
`PKT_FRAME` chunks over the same protocol the rest of the project uses; the
server reassembles it, runs a 3×3 filter (Sobel edge detection by default), and
writes the result as a BMP — with no image library anywhere.

```
device --PKT_FRAME chunks--> telemetryd --reassemble--> Sobel --> dev<id>_edges.bmp
```

## Files
- `image.{c,h}` — an 8-bit grayscale image (`pix[y*w + x]`) + a synthetic test
  pattern (gradient, a box, a diagonal) so edges are obvious and checkable.
- `bmp.{c,h}` — hand-rolled 8-bit grayscale BMP writer/reader.
- `convolve.{c,h}` — generic 3×3 convolution (edge-clamped) + Sobel magnitude,
  with named kernels (identity, blur, sharpen).
- `frame.{c,h}` — `PKT_FRAME` chunk builder + a defensive reassembler.
- `pl_imaging.c` — demo: synthesize a frame, write input/edges/blur/sharpen BMPs,
  and round-trip the frame through chunking.
- `img_test.c` — host tests.

## Run it
```
make test           # BMP round-trip, identity conv, Sobel edge, frame reassembly
make demo           # writes frame_{input,edges,blur,sharpen}.bmp here
./pl_imaging /tmp   # or pick an output dir
```
Live, through the server (on a Linux VM or any host):
```
../server/telemetryd -F ./frames &                  # -F enables frame BMP output
../tools/fake_client -h 127.0.0.1 -p 9000 -d 0x1001 --frame
ls frames/                                           # dev00001001_in.bmp + _edges.bmp
```

## Design notes (interview talking points)
- **Hand-rolled BMP, no libpng/libjpeg.** Writing the format by hand shows it: a
  14-byte file header, a 40-byte DIB header, a 256-entry grayscale palette, then
  rows stored **bottom-up** and padded to a **4-byte boundary**. All fields are
  written as explicit little-endian bytes — the mirror image of the big-endian
  wire code, and correct regardless of host endianness. BMP was chosen precisely
  because it's uncompressed: no DEFLATE, no DCT, just headers and pixels.
- **Edge-clamped convolution.** Out-of-bounds taps replicate the nearest edge
  pixel, so the output is the same size as the input with no dark border —
  cheaper and simpler than padding, and fine for edge detection.
- **Sobel = two kernels + magnitude.** `sqrt(Gx² + Gy²)` clamped to a byte. The
  same `conv3x3` engine also does blur/sharpen; Sobel is special-cased only
  because it combines two passes.
- **Frames ride the existing protocol.** A frame is `ceil(w*h / 1024)` chunks,
  each a `PKT_FRAME` packet (the same 12-byte header + checksum as everything
  else, then a 12-byte `pl_frame_header_t` + chunk bytes). No second transport.
- **The reassembler is defensive.** It validates dimensions against a pixel cap,
  checks that `chunk_count` matches `ceil(w*h/1024)`, that each chunk's index and
  exact length are right, and dedupes repeats — a malformed or hostile stream is
  rejected, not trusted. The same `frame_asm` runs in `img_test` and inside the
  real `client_worker` (proven by `server/frame_e2e_test`).
