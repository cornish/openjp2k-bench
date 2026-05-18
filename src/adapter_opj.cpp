// Shared adapter for both openjpeg and openjp2k. Each decoder owns its own
// dlopen'd OpjApi table pointing at a separately-built .so, so the two
// libraries' identical opj_* symbol sets stay isolated. See opj_dyn.h for
// the isolation strategy.
//
// Note on --reuse-codec: this adapter does NOT override Decoder::prepare().
// OpenJPEG's codec is consumed by opj_decode + opj_end_decompress and has no
// public reset API; reusing the same opj_codec_t* across iterations is not
// documented and observed to crash on second decode. Under --reuse-codec the
// base OneShotPrepared wrapper falls back to per-iter codec recreation, so
// these adapters' reuse-mode numbers match one-shot mode and `reused_codec`
// stays false for their rows.

#include "adapter.h"
#include "opj_dyn.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>

namespace jp2kbench {
namespace {

struct MemStream {
  const uint8_t* data;
  std::size_t size;
  std::size_t offset;
};

OPJ_SIZE_T mem_read(void* p_buffer, OPJ_SIZE_T n, void* user) {
  auto* s = static_cast<MemStream*>(user);
  if (s->offset >= s->size) return (OPJ_SIZE_T)-1;
  std::size_t remaining = s->size - s->offset;
  std::size_t to_read = n < remaining ? n : remaining;
  std::memcpy(p_buffer, s->data + s->offset, to_read);
  s->offset += to_read;
  return to_read;
}

OPJ_OFF_T mem_skip(OPJ_OFF_T n, void* user) {
  auto* s = static_cast<MemStream*>(user);
  if (n < 0) return -1;
  std::size_t remaining = s->size - s->offset;
  std::size_t to_skip = (std::size_t)n < remaining ? (std::size_t)n : remaining;
  s->offset += to_skip;
  return (OPJ_OFF_T)to_skip;
}

OPJ_BOOL mem_seek(OPJ_OFF_T n, void* user) {
  auto* s = static_cast<MemStream*>(user);
  if (n < 0 || (std::size_t)n > s->size) return OPJ_FALSE;
  s->offset = (std::size_t)n;
  return OPJ_TRUE;
}

void silent(const char*, void*) {}

// OpenJPEG's error callback dumps a free-form message to its handler. The
// silent handler above throws those away, which collapsed many failures to
// just "decode" with no detail. Mirror the grok adapter's pattern: capture
// the last error in a thread_local and append it to the stage label on
// failure. thread_local keeps openjpeg-vs-openjp2k decodes on different
// threads from clobbering each other; on the bench's serialized decode
// path that's belt-and-suspenders.
thread_local std::string g_last_opj_error;

void opj_error_capture(const char* msg, void*) {
  if (!msg) return;
  std::size_t n = std::strlen(msg);
  while (n > 0 && (msg[n-1] == '\n' || msg[n-1] == '\r')) --n;
  g_last_opj_error.assign(msg, n);
}

bool opj_fail(std::string& err, const char* stage) {
  err = stage;
  if (!g_last_opj_error.empty()) { err += ": "; err += g_last_opj_error; }
  return false;
}

OPJ_CODEC_FORMAT sniff_codec(const uint8_t* data, std::size_t size) {
  static const uint8_t jp2_sig[] = {0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ',
                                    0x0D, 0x0A, 0x87, 0x0A};
  if (size >= sizeof(jp2_sig) && std::memcmp(data, jp2_sig, sizeof(jp2_sig)) == 0)
    return OPJ_CODEC_JP2;
  if (size >= 4 && data[0] == 0xFF && data[1] == 0x4F &&
      data[2] == 0xFF && data[3] == 0x51)
    return OPJ_CODEC_J2K;
  return OPJ_CODEC_UNKNOWN;
}

double now_seconds() {
  using clk = std::chrono::steady_clock;
  static const auto t0 = clk::now();
  return std::chrono::duration<double>(clk::now() - t0).count();
}

bool unpack_opj_image(const opj_image_t* image, DecodedImage& out, std::string& err) {
  if (!image || image->numcomps == 0 || !image->comps) {
    err = "empty image"; return false;
  }
  uint32_t w = image->comps[0].w;
  uint32_t h = image->comps[0].h;
  uint32_t prec = image->comps[0].prec;
  for (uint32_t c = 1; c < image->numcomps; ++c) {
    if (image->comps[c].w != w || image->comps[c].h != h) {
      err = "non-uniform component size (subsampled YUV not supported)";
      return false;
    }
  }
  out.width = w;
  out.height = h;
  out.channels = image->numcomps;
  out.bit_depth = prec;

  if (prec <= 8) {
    out.pixels.resize((std::size_t)w * h * image->numcomps);
    for (uint32_t c = 0; c < image->numcomps; ++c) {
      const OPJ_INT32* src = image->comps[c].data;
      int sgnd = image->comps[c].sgnd;
      int shift = sgnd ? (1 << (prec - 1)) : 0;
      uint8_t* dst = out.pixels.data() + c;
      for (uint32_t i = 0, n = w * h; i < n; ++i) {
        int v = src[i] + shift;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        dst[i * image->numcomps] = (uint8_t)v;
      }
    }
  } else {
    out.pixels.resize((std::size_t)w * h * image->numcomps * 2);
    uint32_t max = (1u << prec) - 1;
    for (uint32_t c = 0; c < image->numcomps; ++c) {
      const OPJ_INT32* src = image->comps[c].data;
      int sgnd = image->comps[c].sgnd;
      int shift = sgnd ? (1 << (prec - 1)) : 0;
      uint8_t* dst = out.pixels.data() + c * 2;
      for (uint32_t i = 0, n = w * h; i < n; ++i) {
        int v = src[i] + shift;
        if (v < 0) v = 0; else if ((uint32_t)v > max) v = (int)max;
        uint16_t u = (uint16_t)v;
        dst[i * image->numcomps * 2 + 0] = (uint8_t)(u & 0xFF);
        dst[i * image->numcomps * 2 + 1] = (uint8_t)(u >> 8);
      }
    }
  }
  return true;
}

class OpjDynDecoder : public Decoder {
 public:
  OpjDynDecoder(const char* name, const OpjApi& api)
      : name_(name), api_(api) {}

  const char* name() const override { return name_; }
  std::string version() const override { return api_.version(); }

  bool native_region_decode() const override { return true; }

  bool header_only(const uint8_t* data, std::size_t size, int num_threads,
                   std::string& err) override {
    g_last_opj_error.clear();
    OPJ_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == OPJ_CODEC_UNKNOWN) { err = "unknown codestream"; return false; }
    MemStream mem{data, size, 0};
    opj_stream_t* stream = api_.stream_default_create(OPJ_TRUE);
    api_.stream_set_read_function(stream, &mem_read);
    api_.stream_set_skip_function(stream, &mem_skip);
    api_.stream_set_seek_function(stream, &mem_seek);
    api_.stream_set_user_data(stream, &mem, nullptr);
    api_.stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_codec_t* codec = api_.create_decompress(fmt);
    if (!codec) { api_.stream_destroy(stream); err = "create_decompress"; return false; }
    api_.set_info_handler(codec, &silent, nullptr);
    api_.set_warning_handler(codec, &silent, nullptr);
    api_.set_error_handler(codec, &opj_error_capture, nullptr);
    opj_dparameters_t params;
    api_.set_default_decoder_parameters(&params);
    if (!api_.setup_decoder(codec, &params)) {
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "setup_decoder");
    }
    if (num_threads > 1) api_.codec_set_threads(codec, num_threads);
    opj_image_t* image = nullptr;
    bool ok = api_.read_header(stream, codec, &image);
    if (image) api_.image_destroy(image);
    api_.destroy_codec(codec);
    api_.stream_destroy(stream);
    if (!ok) return opj_fail(err, "read_header");
    return true;
  }

  bool decode_with_stages(const uint8_t* data, std::size_t size, int num_threads,
                          DecodedImage& out, StageTimings& stages,
                          std::string& err) override {
    stages = {};
    double ts0 = now_seconds();
    g_last_opj_error.clear();
    OPJ_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == OPJ_CODEC_UNKNOWN) { err = "unknown codestream"; return false; }
    MemStream mem{data, size, 0};
    opj_stream_t* stream = api_.stream_default_create(OPJ_TRUE);
    api_.stream_set_read_function(stream, &mem_read);
    api_.stream_set_skip_function(stream, &mem_skip);
    api_.stream_set_seek_function(stream, &mem_seek);
    api_.stream_set_user_data(stream, &mem, nullptr);
    api_.stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_codec_t* codec = api_.create_decompress(fmt);
    if (!codec) { api_.stream_destroy(stream); err = "create_decompress"; return false; }
    api_.set_info_handler(codec, &silent, nullptr);
    api_.set_warning_handler(codec, &silent, nullptr);
    api_.set_error_handler(codec, &opj_error_capture, nullptr);
    opj_dparameters_t params;
    api_.set_default_decoder_parameters(&params);
    if (!api_.setup_decoder(codec, &params)) {
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "setup_decoder");
    }
    if (num_threads > 1) api_.codec_set_threads(codec, num_threads);
    opj_image_t* image = nullptr;
    if (!api_.read_header(stream, codec, &image)) {
      if (image) api_.image_destroy(image);
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "read_header");
    }
    double ts1 = now_seconds();
    stages.setup_s = ts1 - ts0;

    if (!api_.decode(codec, stream, image) ||
        !api_.end_decompress(codec, stream)) {
      api_.image_destroy(image);
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "decode");
    }
    double ts2 = now_seconds();
    stages.decode_s = ts2 - ts1;

    bool ok = unpack_opj_image(image, out, err);
    double ts3 = now_seconds();
    stages.unpack_s = ts3 - ts2;

    api_.image_destroy(image);
    api_.destroy_codec(codec);
    api_.stream_destroy(stream);
    stages.teardown_s = now_seconds() - ts3;
    return ok;
  }

  bool decode(const uint8_t* data, std::size_t size, int num_threads,
              DecodedImage& out, std::string& err) override {
    g_last_opj_error.clear();
    OPJ_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == OPJ_CODEC_UNKNOWN) { err = "unknown codestream"; return false; }
    MemStream mem{data, size, 0};
    opj_stream_t* stream = api_.stream_default_create(OPJ_TRUE);
    api_.stream_set_read_function(stream, &mem_read);
    api_.stream_set_skip_function(stream, &mem_skip);
    api_.stream_set_seek_function(stream, &mem_seek);
    api_.stream_set_user_data(stream, &mem, nullptr);
    api_.stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_codec_t* codec = api_.create_decompress(fmt);
    if (!codec) { api_.stream_destroy(stream); err = "create_decompress"; return false; }
    api_.set_info_handler(codec, &silent, nullptr);
    api_.set_warning_handler(codec, &silent, nullptr);
    api_.set_error_handler(codec, &opj_error_capture, nullptr);
    opj_dparameters_t params;
    api_.set_default_decoder_parameters(&params);
    if (!api_.setup_decoder(codec, &params)) {
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "setup_decoder");
    }
    if (num_threads > 1) api_.codec_set_threads(codec, num_threads);
    opj_image_t* image = nullptr;
    if (!api_.read_header(stream, codec, &image)) {
      if (image) api_.image_destroy(image);
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "read_header");
    }
    if (!api_.decode(codec, stream, image) ||
        !api_.end_decompress(codec, stream)) {
      api_.image_destroy(image);
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "decode");
    }
    api_.destroy_codec(codec);
    api_.stream_destroy(stream);
    bool ok = unpack_opj_image(image, out, err);
    api_.image_destroy(image);
    return ok;
  }

  bool decode_region(const uint8_t* data, std::size_t size, int num_threads,
                     const Region& region, DecodedImage& out,
                     std::string& err) override {
    g_last_opj_error.clear();
    OPJ_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == OPJ_CODEC_UNKNOWN) { err = "unknown codestream"; return false; }
    MemStream mem{data, size, 0};
    opj_stream_t* stream = api_.stream_default_create(OPJ_TRUE);
    api_.stream_set_read_function(stream, &mem_read);
    api_.stream_set_skip_function(stream, &mem_skip);
    api_.stream_set_seek_function(stream, &mem_seek);
    api_.stream_set_user_data(stream, &mem, nullptr);
    api_.stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_codec_t* codec = api_.create_decompress(fmt);
    if (!codec) { api_.stream_destroy(stream); err = "create_decompress"; return false; }
    api_.set_info_handler(codec, &silent, nullptr);
    api_.set_warning_handler(codec, &silent, nullptr);
    api_.set_error_handler(codec, &opj_error_capture, nullptr);
    opj_dparameters_t params;
    api_.set_default_decoder_parameters(&params);
    if (!api_.setup_decoder(codec, &params)) {
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "setup_decoder");
    }
    if (num_threads > 1) api_.codec_set_threads(codec, num_threads);
    opj_image_t* image = nullptr;
    if (!api_.read_header(stream, codec, &image)) {
      if (image) api_.image_destroy(image);
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "read_header");
    }
    OPJ_INT32 x0 = (OPJ_INT32)region.x0, y0 = (OPJ_INT32)region.y0;
    OPJ_INT32 x1 = region.x1 ? (OPJ_INT32)region.x1 : (OPJ_INT32)image->x1;
    OPJ_INT32 y1 = region.y1 ? (OPJ_INT32)region.y1 : (OPJ_INT32)image->y1;
    if (!api_.set_decode_area(codec, image, x0, y0, x1, y1)) {
      api_.image_destroy(image);
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "set_decode_area");
    }
    if (!api_.decode(codec, stream, image) ||
        !api_.end_decompress(codec, stream)) {
      api_.image_destroy(image);
      api_.destroy_codec(codec); api_.stream_destroy(stream);
      return opj_fail(err, "decode");
    }
    api_.destroy_codec(codec);
    api_.stream_destroy(stream);
    bool ok = unpack_opj_image(image, out, err);
    api_.image_destroy(image);
    return ok;
  }

 private:
  const char* name_;
  const OpjApi& api_;
};

// One OpjApi per .so, loaded on first request and held for the lifetime of
// the process. Returned by reference so OpjDynDecoder instances stay tiny.
// std::call_once gives thread-safe single-shot init; if loading fails we
// stash the error in the OpjApi's handle (left null) and the factory
// surfaces it.
struct LoadedLib {
  OpjApi api{};
  std::string err;
  bool loaded = false;
};

LoadedLib& openjpeg_lib() {
  static LoadedLib L;
  static std::once_flag f;
  std::call_once(f, [] {
    L.loaded = load_opj_api(JP2KBENCH_OPENJPEG_SO, L.api, L.err);
  });
  return L;
}

LoadedLib& openjp2k_lib() {
  static LoadedLib L;
  static std::once_flag f;
  std::call_once(f, [] {
    L.loaded = load_opj_api(JP2KBENCH_OPENJP2K_SO, L.api, L.err);
  });
  return L;
}

std::unique_ptr<Decoder> make_from(const char* name, LoadedLib& lib) {
  if (!lib.loaded) {
    std::cerr << "fatal: could not load " << name << ": " << lib.err << "\n";
    return nullptr;
  }
  return std::make_unique<OpjDynDecoder>(name, lib.api);
}

}  // namespace

std::unique_ptr<Decoder> make_openjpeg_decoder() {
  return make_from("openjpeg", openjpeg_lib());
}

std::unique_ptr<Decoder> make_openjp2k_decoder() {
  return make_from("openjp2k", openjp2k_lib());
}

}  // namespace jp2kbench
