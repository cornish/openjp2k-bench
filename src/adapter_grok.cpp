// Grok decoder adapter for Grok v20.x.
//
// Threading model note:
//   Grok v20 takes its worker-thread count at grk_initialize() time and
//   maintains a process-wide pool for the lifetime of the library.  There
//   is no per-codec or per-call API to change the thread count without
//   tearing the pool down (grk_deinitialize + grk_initialize), which would
//   pollute the timed region of the benchmark.
//   // Grok's API exposes a singleton thread pool; per-codec thread tuning
//   // is intentionally not supported.
//
//   We therefore initialize Grok lazily on the first decode() call with the
//   requested thread count.  Subsequent decode() calls with a *different*
//   num_threads will NOT change the actual pool size — the bench's
//   --threads sweep is effectively meaningful only on the first value for
//   the Grok decoder.  This is documented; users running thread sweeps
//   against Grok should run each thread count in a separate process
//   (e.g. one bench invocation per --threads value).
//
// Concurrent-decode safety:
//   Grok v20's public headers do not document whether multiple decode()
//   calls on distinct codec handles are safe to run in parallel from
//   different threads (the shared Taskflow pool described in
//   third_party/grok/src/lib/core/grok.h around grk_initialize complicates
//   this — multiple decodes would contend for the same workers, and there
//   is no header-level guarantee that grk_decompress_init / grk_decompress
//   are reentrant across handles).  Until upstream documents otherwise,
//   we serialize the entire decode() body behind decode_mu_.  For the
//   upcoming --concurrent-files mode (Task 10) this means Grok will
//   effectively run one decode at a time even when the bench dispatches
//   N in parallel; that is the conservative correctness choice.  Revisit
//   if/when Grok upstream documents per-handle concurrency.
//
// Pixel-format contract: must match adapter_openjpeg.cpp.  Interleaved,
// channel-major within a pixel.  prec <= 8 => 1 byte/sample; prec > 8 =>
// 2 bytes/sample, host-endian, right-aligned, unsigned after sign-shift.
// Grok exposes per-component `stride` distinct from `w`; we honor it.

#include "adapter.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

extern "C" {
#include <grok.h>
}

namespace jp2kbench {
namespace {

// Grok v20 message-handler callback signature (see grk_msg_callback typedef
// at third_party/grok/src/lib/core/grok.h:178). Kept separate from the
// OpenJPEG adapter's silent() to avoid sharing a callback across two APIs
// whose typedefs could diverge in future versions.
void grok_silent(const char*, void*) {}

// Thread-local last-error scratchpad. Grok's error_callback fires from
// internal worker threads; we capture the most recent message so decode()
// can surface it via the err out-param when grk_decompress_init / read_header
// / decompress fail. Thread-local keeps it lock-free and avoids cross-call
// contamination under the serialized decode path.
thread_local std::string g_last_grok_error;

void grok_error_capture(const char* msg, void*) {
  if (msg) g_last_grok_error = msg;
}

double now_seconds() {
  using clk = std::chrono::steady_clock;
  static const auto t0 = clk::now();
  return std::chrono::duration<double>(clk::now() - t0).count();
}

// Templated inner loops: with SrcT known at compile time, the per-sample
// load is a single typed read and the surrounding logic vectorizes.
// Previously this lambda had a per-sample switch on grk_data_type which
// was ~15-20% of total decode time on small files.
template <typename SrcT>
void unpack_grok_comp_to_u8(const SrcT* base, uint32_t stride, uint32_t w,
                            uint32_t h, uint32_t nc, uint32_t c, int shift,
                            uint8_t* dst) {
  for (uint32_t y = 0; y < h; ++y) {
    const SrcT* row = base + (std::size_t)y * stride;
    uint8_t* drow = dst + (std::size_t)y * w * nc + c;
    for (uint32_t x = 0; x < w; ++x) {
      int v = (int)row[x] + shift;
      if (v < 0) v = 0; else if (v > 255) v = 255;
      drow[(std::size_t)x * nc] = (uint8_t)v;
    }
  }
}

template <typename SrcT>
void unpack_grok_comp_to_u16(const SrcT* base, uint32_t stride, uint32_t w,
                             uint32_t h, uint32_t nc, uint32_t c,
                             int shift, uint32_t maxv, uint8_t* dst) {
  for (uint32_t y = 0; y < h; ++y) {
    const SrcT* row = base + (std::size_t)y * stride;
    uint8_t* drow = dst + (std::size_t)y * w * nc * 2 + c * 2;
    for (uint32_t x = 0; x < w; ++x) {
      int v = (int)row[x] + shift;
      if (v < 0) v = 0; else if ((uint32_t)v > maxv) v = (int)maxv;
      uint16_t u = (uint16_t)v;
      drow[(std::size_t)x * nc * 2 + 0] = (uint8_t)(u & 0xFF);
      drow[(std::size_t)x * nc * 2 + 1] = (uint8_t)(u >> 8);
    }
  }
}

// Copy a Grok grk_image into out.pixels, planar. Returns false on geometry
// mismatch. Shared by decode() and the staged-timing path.
//
// Grok v20 hands the adapter a planar component buffer whose storage width
// varies per component (GRK_INT_8 / _16 / _32). The format dispatch happens
// ONCE per component — the inner row/column loops are then typed templates
// and the compiler can autovectorize. Outputs planar (each component's
// plane contiguous; components concatenated in order), with bit-depth
// normalization to either u8 (prec <= 8) or u16 little-endian (prec > 8);
// identical contract to the OpenJPEG adapter. The interleave helpers
// (unpack_grok_comp_to_u8/_u16) are reused by passing nc=1, c=0 and
// pointing dst at the per-component plane base.
bool unpack_grok_image(const grk_image* image, DecodedImage& out, std::string& err) {
  if (!image || image->numcomps == 0 || !image->comps) {
    err = "empty image"; return false;
  }
  uint32_t prec = image->comps[0].prec;
  const uint32_t nc = image->numcomps;
  for (uint32_t c = 1; c < nc; ++c) {
    if (image->comps[c].prec != prec) {
      err = "non-uniform component bit depth not supported";
      return false;
    }
  }
  uint32_t canvas_w = image->x1 > image->x0
      ? (image->x1 - image->x0)
      : image->comps[0].w * (image->comps[0].dx ? image->comps[0].dx : 1);
  uint32_t canvas_h = image->y1 > image->y0
      ? (image->y1 - image->y0)
      : image->comps[0].h * (image->comps[0].dy ? image->comps[0].dy : 1);
  out.width = canvas_w; out.height = canvas_h;
  out.channels = nc; out.bit_depth = prec;
  out.components.clear();
  out.components.reserve(nc);
  for (uint32_t c = 0; c < nc; ++c) {
    ComponentDims cd;
    cd.w = image->comps[c].w;
    cd.h = image->comps[c].h;
    cd.dx = image->comps[c].dx ? image->comps[c].dx : 1;
    cd.dy = image->comps[c].dy ? image->comps[c].dy : 1;
    out.components.push_back(cd);
  }

  std::size_t bps = out.bytes_per_sample();
  std::size_t total = 0;
  for (const auto& cd : out.components) total += (std::size_t)cd.w * cd.h * bps;
  out.pixels.resize(total);

  std::size_t plane_off = 0;
  if (prec <= 8) {
    for (uint32_t c = 0; c < nc; ++c) {
      const grk_image_comp& comp = image->comps[c];
      const uint32_t cw = comp.w;
      const uint32_t ch = comp.h;
      const uint32_t stride = comp.stride ? comp.stride : cw;
      const int shift = comp.sgnd ? (1 << (prec - 1)) : 0;
      uint8_t* dst = out.pixels.data() + plane_off;
      switch (comp.data_type) {
        case GRK_INT_8:
          unpack_grok_comp_to_u8<int8_t>(
              static_cast<const int8_t*>(comp.data), stride, cw, ch, 1, 0, shift, dst);
          break;
        case GRK_INT_16:
          unpack_grok_comp_to_u8<int16_t>(
              static_cast<const int16_t*>(comp.data), stride, cw, ch, 1, 0, shift, dst);
          break;
        case GRK_INT_32:
        default:
          unpack_grok_comp_to_u8<int32_t>(
              static_cast<const int32_t*>(comp.data), stride, cw, ch, 1, 0, shift, dst);
          break;
      }
      plane_off += (std::size_t)cw * ch;
    }
  } else {
    const uint32_t maxv = (1u << prec) - 1;
    for (uint32_t c = 0; c < nc; ++c) {
      const grk_image_comp& comp = image->comps[c];
      const uint32_t cw = comp.w;
      const uint32_t ch = comp.h;
      const uint32_t stride = comp.stride ? comp.stride : cw;
      const int shift = comp.sgnd ? (1 << (prec - 1)) : 0;
      uint8_t* dst = out.pixels.data() + plane_off;
      switch (comp.data_type) {
        case GRK_INT_8:
          unpack_grok_comp_to_u16<int8_t>(
              static_cast<const int8_t*>(comp.data), stride, cw, ch, 1, 0, shift, maxv, dst);
          break;
        case GRK_INT_16:
          unpack_grok_comp_to_u16<int16_t>(
              static_cast<const int16_t*>(comp.data), stride, cw, ch, 1, 0, shift, maxv, dst);
          break;
        case GRK_INT_32:
        default:
          unpack_grok_comp_to_u16<int32_t>(
              static_cast<const int32_t*>(comp.data), stride, cw, ch, 1, 0, shift, maxv, dst);
          break;
      }
      plane_off += (std::size_t)cw * ch * 2;
    }
  }
  return true;
}

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

  bool native_region_decode() const override { return false; }
  // decode_region uses the base-class default (full decode + crop).

  bool header_only(const uint8_t* data, std::size_t size, int num_threads,
                   std::string& err) override {
    std::lock_guard<std::mutex> decode_lk(decode_mu_);
    if (!ensure_initialized(num_threads)) {
      err = "grk_initialize failed"; return false;
    }
    GRK_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == GRK_CODEC_UNK) { err = "unknown codestream"; return false; }
    grk_decompress_parameters params{};
    grk_stream_params sp{};
    sp.buf = const_cast<uint8_t*>(data);
    sp.buf_len = size;
    g_last_grok_error.clear();
    grk_object* codec = grk_decompress_init(&sp, &params);
    if (!codec) {
      err = "decompress_init";
      if (!g_last_grok_error.empty()) { err += ": "; err += g_last_grok_error; }
      return false;
    }
    grk_header_info hdr{};
    bool ok = grk_decompress_read_header(codec, &hdr);
    grk_object_unref(codec);
    if (!ok) {
      err = "read_header";
      if (!g_last_grok_error.empty()) { err += ": "; err += g_last_grok_error; }
      return false;
    }
    return true;
  }

  bool decode_with_stages(const uint8_t* data, std::size_t size, int num_threads,
                          DecodedImage& out, StageTimings& stages,
                          std::string& err) override {
    stages = {};
    std::lock_guard<std::mutex> decode_lk(decode_mu_);
    double ts0 = now_seconds();
    if (!ensure_initialized(num_threads)) {
      err = "grk_initialize failed"; return false;
    }
    GRK_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == GRK_CODEC_UNK) { err = "unknown codestream"; return false; }
    grk_decompress_parameters params{};
    grk_stream_params sp{};
    sp.buf = const_cast<uint8_t*>(data);
    sp.buf_len = size;
    g_last_grok_error.clear();
    grk_object* codec = grk_decompress_init(&sp, &params);
    if (!codec) {
      err = "decompress_init";
      if (!g_last_grok_error.empty()) { err += ": "; err += g_last_grok_error; }
      return false;
    }
    grk_header_info hdr{};
    if (!grk_decompress_read_header(codec, &hdr)) {
      grk_object_unref(codec);
      err = "read_header";
      if (!g_last_grok_error.empty()) { err += ": "; err += g_last_grok_error; }
      return false;
    }
    double ts1 = now_seconds();
    stages.setup_s = ts1 - ts0;

    if (!grk_decompress(codec, nullptr)) {
      grk_object_unref(codec);
      err = "decompress";
      if (!g_last_grok_error.empty()) { err += ": "; err += g_last_grok_error; }
      return false;
    }
    grk_image* image = grk_decompress_get_image(codec);
    double ts2 = now_seconds();
    stages.decode_s = ts2 - ts1;

    bool ok = unpack_grok_image(image, out, err);
    double ts3 = now_seconds();
    stages.unpack_s = ts3 - ts2;

    grk_object_unref(codec);
    stages.teardown_s = now_seconds() - ts3;
    return ok;
  }

  bool decode(const uint8_t* data, std::size_t size, int num_threads,
              DecodedImage& out, std::string& err) override {
    // See top-of-file "Concurrent-decode safety" note for why decode() is
    // serialized in its entirety.
    std::lock_guard<std::mutex> decode_lk(decode_mu_);

    if (!ensure_initialized(num_threads)) {
      err = "grk_initialize failed";
      return false;
    }

    GRK_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == GRK_CODEC_UNK) { err = "unknown codestream"; return false; }

    grk_decompress_parameters params{};

    grk_stream_params sp{};
    sp.buf = const_cast<uint8_t*>(data);
    sp.buf_len = size;

    g_last_grok_error.clear();

    grk_object* codec = grk_decompress_init(&sp, &params);
    if (!codec) {
      err = "decompress_init";
      if (!g_last_grok_error.empty()) {
        err += ": ";
        err += g_last_grok_error;
      }
      return false;
    }

    grk_header_info hdr{};
    if (!grk_decompress_read_header(codec, &hdr)) {
      grk_object_unref(codec);
      err = "read_header";
      if (!g_last_grok_error.empty()) { err += ": "; err += g_last_grok_error; }
      return false;
    }

    if (!grk_decompress(codec, nullptr)) {
      grk_object_unref(codec);
      err = "decompress";
      if (!g_last_grok_error.empty()) { err += ": "; err += g_last_grok_error; }
      return false;
    }

    grk_image* image = grk_decompress_get_image(codec);
    bool ok = unpack_grok_image(image, out, err);
    grk_object_unref(codec);
    return ok;
  }

  // supports_codec_reuse stays at the base-class default (false): empirical
  // test on v20.3.3 — calling grk_decompress() a second time on a codec
  // produced by grk_decompress_init() + grk_decompress_read_header()
  // SEGVs. The set_progression_state documentation talks about re-decode,
  // but only for changed layer budgets via the cache, not idempotent
  // re-decode of the same content. So --reuse-codec for Grok falls back
  // to the base OneShotPrepared and yields one-shot numbers.

 private:
  // Returns true on success.  grk_initialize() itself returns void in v20
  // (verified at third_party/grok/src/lib/core/grok.h:1175), so there is
  // no init-failure status to propagate from that call directly; we still
  // return bool to leave a hook for a future API change and for symmetry
  // with the decode() error path.
  bool ensure_initialized(int num_threads) {
    std::lock_guard<std::mutex> lk(init_mu_);
    if (initialized_) return true;
    uint32_t t = num_threads > 0 ? (uint32_t)num_threads : 0;  // 0 = all CPUs
    grk_msg_handlers handlers{};
    handlers.info_callback  = &grok_silent;
    handlers.debug_callback = &grok_silent;
    handlers.trace_callback = &grok_silent;
    handlers.warn_callback  = &grok_silent;
    handlers.error_callback = &grok_error_capture;
    grk_set_msg_handlers(handlers);
    grk_initialize(nullptr, t, nullptr);
    initialized_ = true;
    return true;
  }

  std::mutex init_mu_;
  std::mutex decode_mu_;
  bool initialized_ = false;
};

}  // namespace

std::unique_ptr<Decoder> make_grok_decoder() {
  return std::make_unique<GrokDecoder>();
}

}  // namespace jp2kbench
