// OpenJPEG decoder adapter. Uses the in-memory stream pattern documented in
// openjpeg's examples — openjpeg ships no built-in memory stream.

#include "adapter.h"

#include <cstring>
#include <cstdio>

extern "C" {
#include <openjpeg.h>
}

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

// Errors are noisy by default and pollute timing output; suppress to caller
// buffer instead.
void silent(const char*, void*) {}

OPJ_CODEC_FORMAT sniff_codec(const uint8_t* data, std::size_t size) {
  // JP2 signature box starts with 0x0000000C "jP  " \r\n 0x87 \n
  static const uint8_t jp2_sig[] = {0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ',
                                    0x0D, 0x0A, 0x87, 0x0A};
  if (size >= sizeof(jp2_sig) && std::memcmp(data, jp2_sig, sizeof(jp2_sig)) == 0) {
    return OPJ_CODEC_JP2;
  }
  // Raw J2K codestream starts with SOC marker 0xFF 0x4F followed by SIZ 0xFF 0x51
  if (size >= 4 && data[0] == 0xFF && data[1] == 0x4F &&
      data[2] == 0xFF && data[3] == 0x51) {
    return OPJ_CODEC_J2K;
  }
  return OPJ_CODEC_UNKNOWN;
}

class OpenJpegDecoder : public Decoder {
 public:
  const char* name() const override { return "openjpeg"; }
  std::string version() const override { return opj_version(); }

  bool decode(const uint8_t* data, std::size_t size, int num_threads,
              DecodedImage& out, std::string& err) override {
    OPJ_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == OPJ_CODEC_UNKNOWN) { err = "unknown codestream"; return false; }

    MemStream mem{data, size, 0};
    opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE /* is_read */);
    opj_stream_set_read_function(stream, &mem_read);
    opj_stream_set_skip_function(stream, &mem_skip);
    opj_stream_set_seek_function(stream, &mem_seek);
    opj_stream_set_user_data(stream, &mem, nullptr);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)size);

    opj_codec_t* codec = opj_create_decompress(fmt);
    if (!codec) { opj_stream_destroy(stream); err = "create_decompress"; return false; }
    opj_set_info_handler(codec, &silent, nullptr);
    opj_set_warning_handler(codec, &silent, nullptr);
    opj_set_error_handler(codec, &silent, nullptr);

    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    if (!opj_setup_decoder(codec, &params)) {
      opj_destroy_codec(codec); opj_stream_destroy(stream);
      err = "setup_decoder"; return false;
    }

    if (num_threads > 1) {
      opj_codec_set_threads(codec, num_threads);
    }

    opj_image_t* image = nullptr;
    if (!opj_read_header(stream, codec, &image)) {
      if (image) opj_image_destroy(image);
      opj_destroy_codec(codec); opj_stream_destroy(stream);
      err = "read_header"; return false;
    }

    if (!opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
      opj_image_destroy(image);
      opj_destroy_codec(codec); opj_stream_destroy(stream);
      err = "decode"; return false;
    }

    opj_destroy_codec(codec);
    opj_stream_destroy(stream);

    if (!image || image->numcomps == 0 || !image->comps) {
      if (image) opj_image_destroy(image);
      err = "empty image"; return false;
    }

    // Validate uniform component geometry (we don't handle YUV subsampling).
    uint32_t w = image->comps[0].w;
    uint32_t h = image->comps[0].h;
    uint32_t prec = image->comps[0].prec;
    for (uint32_t c = 1; c < image->numcomps; ++c) {
      if (image->comps[c].w != w || image->comps[c].h != h) {
        opj_image_destroy(image);
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

    opj_image_destroy(image);
    return true;
  }
};

}  // namespace

std::unique_ptr<Decoder> make_openjpeg_decoder() {
  return std::make_unique<OpenJpegDecoder>();
}

}  // namespace jp2kbench
