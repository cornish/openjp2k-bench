#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace jp2kbench {

struct DecodedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;
  uint32_t bit_depth = 0;
  // Interleaved, channel-major within a pixel. 8bpc => one byte per sample;
  // >8bpc => two bytes per sample (host-endian, right-aligned).
  std::vector<uint8_t> pixels;

  // Equality is exact for lossless and unhelpful for lossy. Callers that
  // need lossy comparison should compute PSNR against a ground-truth image.
  bool same_shape(const DecodedImage& o) const {
    return width == o.width && height == o.height &&
           channels == o.channels && bit_depth == o.bit_depth;
  }
};

class Decoder;

// Per-stage timing breakdown of one decode call. All in seconds; zero
// means "not measured by this adapter on this code path." Use only the
// non-zero fields. Sum is not exhaustive — uncategorized overhead is the
// difference between the iter's wall time and (setup+decode+unpack+teardown).
struct StageTimings {
  double setup_s    = 0.0;   // create_decompress + setup + read_header
  double decode_s   = 0.0;   // opj_decode / grk_decompress proper
  double unpack_s   = 0.0;   // adapter-side interleave into DecodedImage
  double teardown_s = 0.0;   // destroy / unref
};

// Opaque handle returned by Decoder::prepare(). Lifetime: created outside
// the timed loop; decode() invoked N times inside it. Whatever per-iter
// setup cost the adapter can hoist out of decode() is hoisted into the
// prepare() that produced this object. Adapters that genuinely cannot
// hoist anything (e.g. codec is single-use) return a wrapper that just
// reruns Decoder::decode() each call; the timed numbers will then match
// one-shot mode for that adapter, which is informative on its own.
class PreparedDecode {
 public:
  virtual ~PreparedDecode() = default;
  virtual bool decode(int num_threads, DecodedImage& out, std::string& err) = 0;
};

class Decoder {
 public:
  virtual ~Decoder() = default;
  virtual const char* name() const = 0;
  virtual std::string version() const = 0;
  // num_threads <= 1 means single-threaded. Decoders that can't honor the
  // request should still produce correct output and report what they used.
  virtual bool decode(const uint8_t* data, std::size_t size, int num_threads,
                      DecodedImage& out, std::string& err) = 0;

  struct Region {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // half-open [x0,x1) × [y0,y1)
  };

  // Decode only the given region. Default = full decode + crop; adapters
  // override to call native region APIs.
  virtual bool decode_region(const uint8_t* data, std::size_t size,
                             int num_threads, const Region& region,
                             DecodedImage& out, std::string& err);

  virtual bool native_region_decode() const { return false; }

  // Build a PreparedDecode that hoists per-iter setup. Default = trivial
  // wrapper that reruns decode() each call (no hoisting). Adapters that
  // can reuse their codec handle should override.
  virtual std::unique_ptr<PreparedDecode> prepare(
      const uint8_t* data, std::size_t size, std::string& err);

  // True if prepare() actually hoists nontrivial work. Reported in JSON
  // so consumers can distinguish "we measured per-iter setup amortization"
  // from "this adapter has no reuse path."
  virtual bool supports_codec_reuse() const { return false; }

  // Setup-only pass: create codec, parse header, destroy. Used by --header-
  // only mode as a proxy for per-iter codec setup cost. Adapters override
  // when they can do this without invoking the full decode path.
  virtual bool header_only(const uint8_t* data, std::size_t size,
                           int num_threads, std::string& err) = 0;

  // Full decode with per-stage timings populated. Default = run decode()
  // and leave all stages at zero (the timed total is still in the bench's
  // outer timer). Adapters override to bracket their internal phases.
  virtual bool decode_with_stages(const uint8_t* data, std::size_t size,
                                  int num_threads, DecodedImage& out,
                                  StageTimings& stages, std::string& err) {
    stages = {};
    return decode(data, size, num_threads, out, err);
  }
};

inline bool Decoder::decode_region(const uint8_t* data, std::size_t size,
                                   int num_threads, const Region& region,
                                   DecodedImage& out, std::string& err) {
  if (!decode(data, size, num_threads, out, err)) return false;
  uint32_t x0 = region.x0, y0 = region.y0;
  uint32_t x1 = region.x1 == 0 ? out.width  : std::min(region.x1, out.width);
  uint32_t y1 = region.y1 == 0 ? out.height : std::min(region.y1, out.height);
  if (x0 >= x1 || y0 >= y1) { err = "empty region"; return false; }
  uint32_t bw = x1 - x0, bh = y1 - y0;
  uint32_t bpp = (out.bit_depth <= 8 ? 1u : 2u) * out.channels;
  std::vector<uint8_t> cropped((std::size_t)bw * bh * bpp);
  for (uint32_t y = 0; y < bh; ++y) {
    const uint8_t* src = out.pixels.data() + ((y + y0) * out.width + x0) * bpp;
    uint8_t* dst       = cropped.data()     +  y * bw                    * bpp;
    std::memcpy(dst, src, (std::size_t)bw * bpp);
  }
  out.pixels = std::move(cropped);
  out.width  = bw;
  out.height = bh;
  return true;
}

namespace detail {
// Trivial PreparedDecode: rebuilds the codec every call. Used by the default
// Decoder::prepare() so an adapter that doesn't override still works under
// --reuse-codec (just without amortization).
class OneShotPrepared : public PreparedDecode {
 public:
  OneShotPrepared(Decoder* d, const uint8_t* data, std::size_t size)
      : d_(d), data_(data), size_(size) {}
  bool decode(int num_threads, DecodedImage& out, std::string& err) override {
    return d_->decode(data_, size_, num_threads, out, err);
  }
 private:
  Decoder* d_;
  const uint8_t* data_;
  std::size_t size_;
};
}  // namespace detail

inline std::unique_ptr<PreparedDecode> Decoder::prepare(
    const uint8_t* data, std::size_t size, std::string& /*err*/) {
  return std::make_unique<detail::OneShotPrepared>(this, data, size);
}

std::unique_ptr<Decoder> make_openjpeg_decoder();
std::unique_ptr<Decoder> make_openjp2k_decoder();

#if JP2KBENCH_HAVE_GROK
std::unique_ptr<Decoder> make_grok_decoder();
#endif

}  // namespace jp2kbench
