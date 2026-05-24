#pragma once
// Dynamically loaded openjpeg/openjp2k API. Both decoders share one OpjApi
// table; each gets its own dlopen handle so the two libraries' identical
// opj_* symbol sets stay isolated.
//
// We use absolute-path dlopen with RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND:
//  - RTLD_LOCAL stops the loaded symbols from leaking into the global
//    namespace, so loading the second .so does not alias the first.
//  - RTLD_DEEPBIND makes internal calls inside each library resolve to
//    its own copy first, not whichever copy happened to be loaded first.
//  - Absolute path skips the SONAME cache; two files with the same SONAME
//    but different inodes remain distinct link_map entries.

#include <string>

extern "C" {
#include <openjpeg.h>
}

namespace jp2kbench {

struct OpjApi {
  void* handle = nullptr;

  const char*    (*version)();
  opj_stream_t*  (*stream_default_create)(OPJ_BOOL is_input);
  void           (*stream_set_read_function)(opj_stream_t*, opj_stream_read_fn);
  void           (*stream_set_skip_function)(opj_stream_t*, opj_stream_skip_fn);
  void           (*stream_set_seek_function)(opj_stream_t*, opj_stream_seek_fn);
  void           (*stream_set_user_data)(opj_stream_t*, void*,
                                         opj_stream_free_user_data_fn);
  void           (*stream_set_user_data_length)(opj_stream_t*, OPJ_UINT64);
  void           (*stream_destroy)(opj_stream_t*);
  opj_codec_t*   (*create_decompress)(OPJ_CODEC_FORMAT);
  void           (*destroy_codec)(opj_codec_t*);
  OPJ_BOOL       (*set_info_handler)(opj_codec_t*, opj_msg_callback, void*);
  OPJ_BOOL       (*set_warning_handler)(opj_codec_t*, opj_msg_callback, void*);
  OPJ_BOOL       (*set_error_handler)(opj_codec_t*, opj_msg_callback, void*);
  void           (*set_default_decoder_parameters)(opj_dparameters_t*);
  OPJ_BOOL       (*setup_decoder)(opj_codec_t*, opj_dparameters_t*);
  OPJ_BOOL       (*decoder_set_strict_mode)(opj_codec_t*, OPJ_BOOL);
  OPJ_BOOL       (*codec_set_threads)(opj_codec_t*, int);
  OPJ_BOOL       (*read_header)(opj_stream_t*, opj_codec_t*, opj_image_t**);
  OPJ_BOOL       (*set_decode_area)(opj_codec_t*, opj_image_t*,
                                    OPJ_INT32, OPJ_INT32,
                                    OPJ_INT32, OPJ_INT32);
  OPJ_BOOL       (*decode)(opj_codec_t*, opj_stream_t*, opj_image_t*);
  OPJ_BOOL       (*end_decompress)(opj_codec_t*, opj_stream_t*);
  void           (*image_destroy)(opj_image_t*);
};

bool load_opj_api(const char* so_path, OpjApi& out, std::string& err);

}  // namespace jp2kbench
