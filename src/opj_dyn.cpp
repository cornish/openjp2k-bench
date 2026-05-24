#include "opj_dyn.h"

#include <dlfcn.h>
#include <cstring>

namespace jp2kbench {

namespace {

template <typename T>
bool resolve(void* handle, const char* sym, T& slot, std::string& err) {
  dlerror();
  void* p = dlsym(handle, sym);
  const char* e = dlerror();
  if (e || !p) {
    err = std::string("dlsym ") + sym + ": " + (e ? e : "null");
    return false;
  }
  // Cast through void* to silence -Wpedantic on function-pointer conversion.
  std::memcpy(&slot, &p, sizeof(slot));
  return true;
}

}  // namespace

bool load_opj_api(const char* so_path, OpjApi& out, std::string& err) {
  void* h = dlopen(so_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (!h) {
    err = std::string("dlopen ") + so_path + ": " + (dlerror() ?: "(no error)");
    return false;
  }
  out.handle = h;

#define R(name, sym) if (!resolve(h, sym, out.name, err)) { dlclose(h); out.handle = nullptr; return false; }
  R(version,                          "opj_version");
  R(stream_default_create,            "opj_stream_default_create");
  R(stream_set_read_function,         "opj_stream_set_read_function");
  R(stream_set_skip_function,         "opj_stream_set_skip_function");
  R(stream_set_seek_function,         "opj_stream_set_seek_function");
  R(stream_set_user_data,             "opj_stream_set_user_data");
  R(stream_set_user_data_length,      "opj_stream_set_user_data_length");
  R(stream_destroy,                   "opj_stream_destroy");
  R(create_decompress,                "opj_create_decompress");
  R(destroy_codec,                    "opj_destroy_codec");
  R(set_info_handler,                 "opj_set_info_handler");
  R(set_warning_handler,              "opj_set_warning_handler");
  R(set_error_handler,                "opj_set_error_handler");
  R(set_default_decoder_parameters,   "opj_set_default_decoder_parameters");
  R(setup_decoder,                    "opj_setup_decoder");
  R(decoder_set_strict_mode,          "opj_decoder_set_strict_mode");
  R(codec_set_threads,                "opj_codec_set_threads");
  R(read_header,                      "opj_read_header");
  R(set_decode_area,                  "opj_set_decode_area");
  R(decode,                           "opj_decode");
  R(end_decompress,                   "opj_end_decompress");
  R(image_destroy,                    "opj_image_destroy");
#undef R
  return true;
}

}  // namespace jp2kbench
