// implementations of the vendored single-header libraries (see src/third_party)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include "third_party/stb_image.h"

#define STB_DXT_IMPLEMENTATION
#include "third_party/stb_dxt.h"

// stb_vorbis for emote sounds (sound.cpp decodes from memory); last, as it leaves its macros and typedefs behind
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "third_party/stb_vorbis.c"

#pragma GCC diagnostic pop
