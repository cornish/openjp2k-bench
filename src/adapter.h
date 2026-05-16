#pragma once
#include <cstddef>
#include <cstdint>
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

class Decoder {
 public:
  virtual ~Decoder() = default;
  virtual const char* name() const = 0;
  virtual std::string version() const = 0;
  // num_threads <= 1 means single-threaded. Decoders that can't honor the
  // request should still produce correct output and report what they used.
  virtual bool decode(const uint8_t* data, std::size_t size, int num_threads,
                      DecodedImage& out, std::string& err) = 0;
};

std::unique_ptr<Decoder> make_openjpeg_decoder();

#if JP2KBENCH_HAVE_GROK
std::unique_ptr<Decoder> make_grok_decoder();
#endif

}  // namespace jp2kbench
