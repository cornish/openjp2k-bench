// Grok decoder adapter. Compiled only when JP2KBENCH_HAVE_GROK is defined,
// which CMake sets when third_party/grok/CMakeLists.txt is present.
//
// Grok's C API (grok.h) mirrors openjpeg's at a high level — memory stream,
// codec, header, decode — but with different types/names.

#include "adapter.h"

#include <cstring>

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
  GrokDecoder() { grk_initialize(nullptr, 0); }
  ~GrokDecoder() override { grk_deinitialize(); }

  const char* name() const override { return "grok"; }
  std::string version() const override { return grk_version(); }

  bool decode(const uint8_t* data, std::size_t size, int num_threads,
              DecodedImage& out, std::string& err) override {
    GRK_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == GRK_CODEC_UNK) { err = "unknown codestream"; return false; }

    grk_set_msg_handlers(silent, nullptr, silent, nullptr, silent, nullptr);

    grk_decompress_parameters params;
    grk_decompress_set_default_params(&params);
    params.numThreads = num_threads > 1 ? num_threads : 1;

    grk_stream_params sp{};
    sp.buf = (uint8_t*)data;
    sp.buf_len = size;

    grk_codec* codec = grk_decompress_init(&sp, &params);
    if (!codec) { err = "decompress_init"; return false; }

    grk_header_info hdr{};
    if (!grk_decompress_read_header(codec, &hdr)) {
      grk_object_unref(codec); err = "read_header"; return false;
    }

    if (!grk_decompress(codec, nullptr)) {
      grk_object_unref(codec); err = "decompress"; return false;
    }

    grk_image* image = grk_decompress_get_composited_image(codec);
    if (!image || image->numcomps == 0) {
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

    if (prec <= 8) {
      out.pixels.resize((std::size_t)w * h * image->numcomps);
      for (uint32_t c = 0; c < image->numcomps; ++c) {
        const int32_t* src = image->comps[c].data;
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
        const int32_t* src = image->comps[c].data;
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

    grk_object_unref(codec);
    return true;
  }
};

}  // namespace

std::unique_ptr<Decoder> make_grok_decoder() {
  return std::make_unique<GrokDecoder>();
}

}  // namespace jp2kbench
