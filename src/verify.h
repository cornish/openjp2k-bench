#pragma once
#include "adapter.h"

namespace jp2kbench {

// PSNR in dB between two same-shape DecodedImages. Returns +inf on exact
// match, NaN if shapes differ.
double psnr_db(const DecodedImage& a, const DecodedImage& b);

}  // namespace jp2kbench
