# tests/fixtures/

Tiny JP2 files used by `tests/smoke.sh` and the Python tests. Checked in so test runs don't depend on the public corpus or `opj_compress` being on `$PATH`.

## tiny.jp2

128² 8-bit RGB, 4:4:4, lossless. Baseline 4:4:4 fixture used by all the existing smoke checks.

## tiny_420.jp2, tiny_422.jp2

128² 8-bit YUV, chroma-subsampled. Exercise the subsampled-component path in `adapter_opj.cpp::unpack_opj_image` and `adapter_grok.cpp::unpack_grok_image`.

| Fixture          | Subsampling | Luma   | Chroma |
|------------------|-------------|--------|--------|
| `tiny_420.jp2`   | 4:2:0       | 128×128| 64×64  |
| `tiny_422.jp2`   | 4:2:2       | 128×128| 64×128 |

Generate from a synthetic YUV blob (deterministic, byte-pattern only — not visually meaningful):

```sh
python3 -c "
W,H=128,128
y  = bytes((i+j)         & 0xFF for j in range(H)   for i in range(W))
cb = bytes((i*4+j*4+64)  & 0xFF for j in range(H//2) for i in range(W//2))
cr = bytes((i*4-j*4+192) & 0xFF for j in range(H//2) for i in range(W//2))
open('/tmp/tiny_420.yuv','wb').write(y+cb+cr)
cb2= bytes((i*4+j*4+64)  & 0xFF for j in range(H)   for i in range(W//2))
cr2= bytes((i*4-j*4+192) & 0xFF for j in range(H)   for i in range(W//2))
open('/tmp/tiny_422.yuv','wb').write(y+cb2+cr2)
"

build-tools/bin/opj_compress -i /tmp/tiny_420.yuv -o tests/fixtures/tiny_420.jp2 -F 128,128,3,8,u@1x1:2x2:2x2
build-tools/bin/opj_compress -i /tmp/tiny_422.yuv -o tests/fixtures/tiny_422.jp2 -F 128,128,3,8,u@1x1:2x1:2x1
```

The `-F` format string is `W,H,components,bitdepth,{s|u}@dx1xdy1:dx2xdy2:...`. The bench's adapter unpackers read each component's `dx`/`dy` from the codestream and emit a per-component `subsampled_components` array in the result row.
