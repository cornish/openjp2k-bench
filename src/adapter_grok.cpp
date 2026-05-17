// Grok decoder adapter for Grok v20.x.
//
// Threading model note:
//   Grok v20 takes its worker-thread count at grk_initialize() time and
//   maintains a process-wide pool for the lifetime of the library.  There
//   is no per-codec or per-call API to change the thread count without
//   tearing the pool down (grk_deinitialize + grk_initialize), which would
//   pollute the timed region of the benchmark.
//
//   We therefore initialize Grok lazily on the first decode() call with the
//   requested thread count.  Subsequent decode() calls with a *different*
//   num_threads will NOT change the actual pool size — the bench's
//   --threads sweep is effectively meaningful only on the first value for
//   the Grok decoder.  This is documented; users running thread sweeps
//   against Grok should run each thread count in a separate process
//   (e.g. one bench invocation per --threads value).
//
// Pixel-format contract: must match adapter_openjpeg.cpp.  Interleaved,
// channel-major within a pixel.  prec <= 8 => 1 byte/sample; prec > 8 =>
// 2 bytes/sample, host-endian, right-aligned, unsigned after sign-shift.
// Grok exposes per-component `stride` distinct from `w`; we honor it.

#include "adapter.h"

#include <cstring>
#include <mutex>

extern "C" {
#include <grok.h>
}

namespace jp2kbench {
namespace {

void silent(const char*, void*) {}

GRK_CODEC_FORMAT sniff_codec(const uint8_t* data, std::size_t size) {
  static const uint8_t jp2_sig[] = {0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ',
                                    0x0D, 0x0A, 0x87, 0x0A};
  if (size >= sizeof(jp2_sig) && std::memcmp(data, jp2_sig, sizeof(jp2_sig)) == 0) {
    return GRK_CODEC_JP2;
  }
  if (size >= 4 && data[0] == 0xFF && data[1] == 0x4F &&
      data[2] == 0xFF && data[3] == 0x51) {
    return GRK_CODEC_J2K;
  }
  return GRK_CODEC_UNK;
}

class GrokDecoder : public Decoder {
 public:
  GrokDecoder() = default;
  ~GrokDecoder() override {
    if (initialized_) {
      grk_deinitialize();
    }
  }

  const char* name() const override { return "grok"; }
  std::string version() const override { return grk_version(); }

  bool decode(const uint8_t* data, std::size_t size, int num_threads,
              DecodedImage& out, std::string& err) override {
    ensure_initialized(num_threads);

    GRK_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == GRK_CODEC_UNK) { err = "unknown codestream"; return false; }

    grk_decompress_parameters params{};

    grk_stream_params sp{};
    sp.buf = const_cast<uint8_t*>(data);
    sp.buf_len = size;

    grk_object* codec = grk_decompress_init(&sp, &params);
    if (!codec) { err = "decompress_init"; return false; }

    grk_header_info hdr{};
    if (!grk_decompress_read_header(codec, &hdr)) {
      grk_object_unref(codec); err = "read_header"; return false;
    }

    if (!grk_decompress(codec, nullptr)) {
      grk_object_unref(codec); err = "decompress"; return false;
    }

    grk_image* image = grk_decompress_get_image(codec);
    if (!image || image->numcomps == 0 || !image->comps) {
      grk_object_unref(codec); err = "empty image"; return false;
    }

    uint32_t w = image->comps[0].w;
    uint32_t h = image->comps[0].h;
    uint32_t prec = image->comps[0].prec;
    for (uint32_t c = 1; c < image->numcomps; ++c) {
      if (image->comps[c].w != w || image->comps[c].h != h) {
        grk_object_unref(codec);
        err = "non-uniform component size";
        return false;
      }
    }

    out.width = w; out.height = h;
    out.channels = image->numcomps;
    out.bit_depth = prec;

    const uint32_t nc = image->numcomps;

    // v20: each component reports its actual storage type (GRK_INT_32,
    // GRK_INT_16, GRK_INT_8).  We must dispatch on it; treating a 16-bit
    // buffer as int32 produces garbage.
    auto load_sample = [&](const grk_image_comp& comp,
                           const void* row_base, uint32_t x) -> int {
      switch (comp.data_type) {
        case GRK_INT_32: return static_cast<const int32_t*>(row_base)[x];
        case GRK_INT_16: return static_cast<const int16_t*>(row_base)[x];
        case GRK_INT_8:  return static_cast<const int8_t*>(row_base)[x];
        default:         return static_cast<const int32_t*>(row_base)[x];
      }
    };
    auto sample_bytes = [](grk_data_type t) -> std::size_t {
      switch (t) {
        case GRK_INT_8:  return 1;
        case GRK_INT_16: return 2;
        case GRK_INT_32: return 4;
        default:         return 4;
      }
    };

    if (prec <= 8) {
      out.pixels.resize((std::size_t)w * h * nc);
      for (uint32_t c = 0; c < nc; ++c) {
        const grk_image_comp& comp = image->comps[c];
        const uint8_t* base = static_cast<const uint8_t*>(comp.data);
        const std::size_t sb = sample_bytes(comp.data_type);
        const uint32_t stride = comp.stride ? comp.stride : w;
        int sgnd = comp.sgnd;
        int shift = sgnd ? (1 << (prec - 1)) : 0;
        uint8_t* dst = out.pixels.data() + c;
        for (uint32_t y = 0; y < h; ++y) {
          const void* row = base + (std::size_t)y * stride * sb;
          for (uint32_t x = 0; x < w; ++x) {
            int v = load_sample(comp, row, x) + shift;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            dst[((std::size_t)y * w + x) * nc] = (uint8_t)v;
          }
        }
      }
    } else {
      out.pixels.resize((std::size_t)w * h * nc * 2);
      uint32_t max = (1u << prec) - 1;
      for (uint32_t c = 0; c < nc; ++c) {
        const grk_image_comp& comp = image->comps[c];
        const uint8_t* base = static_cast<const uint8_t*>(comp.data);
        const std::size_t sb = sample_bytes(comp.data_type);
        const uint32_t stride = comp.stride ? comp.stride : w;
        int sgnd = comp.sgnd;
        int shift = sgnd ? (1 << (prec - 1)) : 0;
        uint8_t* dst = out.pixels.data() + c * 2;
        for (uint32_t y = 0; y < h; ++y) {
          const void* row = base + (std::size_t)y * stride * sb;
          for (uint32_t x = 0; x < w; ++x) {
            int v = load_sample(comp, row, x) + shift;
            if (v < 0) v = 0; else if ((uint32_t)v > max) v = (int)max;
            uint16_t u = (uint16_t)v;
            std::size_t off = ((std::size_t)y * w + x) * nc * 2;
            dst[off + 0] = (uint8_t)(u & 0xFF);
            dst[off + 1] = (uint8_t)(u >> 8);
          }
        }
      }
    }

    grk_object_unref(codec);
    return true;
  }

 private:
  void ensure_initialized(int num_threads) {
    std::lock_guard<std::mutex> lk(init_mu_);
    if (initialized_) return;
    uint32_t t = num_threads > 0 ? (uint32_t)num_threads : 0;  // 0 = all CPUs
    grk_msg_handlers handlers{};
    handlers.info_callback  = &silent;
    handlers.debug_callback = &silent;
    handlers.trace_callback = &silent;
    handlers.warn_callback  = &silent;
    handlers.error_callback = &silent;
    grk_set_msg_handlers(handlers);
    grk_initialize(nullptr, t, nullptr);
    initialized_ = true;
  }

  std::mutex init_mu_;
  bool initialized_ = false;
};

}  // namespace

std::unique_ptr<Decoder> make_grok_decoder() {
  return std::make_unique<GrokDecoder>();
}

}  // namespace jp2kbench
