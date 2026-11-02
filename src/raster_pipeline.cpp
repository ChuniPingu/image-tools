#include "raster_pipeline.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace image_tools {
namespace {

[[noreturn]] void fail_vips(std::string_view operation) {
    std::string message(operation);
    const char* detail = vips_error_buffer();
    if (detail != nullptr && *detail != '\0') {
        message += ": ";
        message += detail;
    }
    vips_error_clear();
    throw std::runtime_error(message);
}

template<class Operation>
Image operate(std::string_view name, Operation operation) {
    VipsImage* raw = nullptr;
    const int status = operation(&raw);
    Image image(raw);
    if (status != 0 || !image) {
        fail_vips(name);
    }
    return image;
}

Image own_image(VipsImage* raw, std::string_view operation) {
    Image image(raw);
    if (!image) {
        fail_vips(operation);
    }
    return image;
}

struct ArrayDeleter {
    void operator()(VipsArrayDouble* value) const noexcept {
        if (value != nullptr) {
            vips_area_unref(reinterpret_cast<VipsArea*>(value));
        }
    }
};

using DoubleArray = std::unique_ptr<VipsArrayDouble, ArrayDeleter>;

DoubleArray background_array(const double* values, int count) {
    auto result = DoubleArray(vips_array_double_new(values, count));
    if (!result) {
        throw std::bad_alloc();
    }
    return result;
}

Image load(const std::filesystem::path& source) {
    if (source.empty()) {
        throw std::invalid_argument("Input path must not be empty.");
    }
    const auto filename = path_to_utf8(source);
    // An empty option block keeps a literal trailing [text] in the filename.
    const auto discovery_name = filename + "[]";
    const char* loader = vips_foreign_find_load(discovery_name.c_str());
    if (loader == nullptr) {
        fail_vips("Unsupported or invalid raster image");
    }
    if (!is_raster_loader(loader)) {
        throw std::runtime_error("Unsupported raster image loader: " + std::string(loader));
    }
    // Direct dispatch avoids parsing the literal path as options a second time.
    // File loaders default to the first page/frame. Never request all pages (n=-1).
    return operate("Load raster image", [&](VipsImage** out) {
        return vips_call(loader, filename.c_str(), out,
            "access", VIPS_ACCESS_SEQUENTIAL, "fail_on", VIPS_FAIL_ON_WARNING,
            "revalidate", TRUE, nullptr);
    });
}

std::optional<double> png_gamma(const std::filesystem::path& source) {
    std::ifstream input(source, std::ios::binary);
    std::array<unsigned char, 8> header{};
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (!input.read(reinterpret_cast<char*>(header.data()), header.size()) || header != signature) {
        return std::nullopt;
    }
    const auto u32 = [](const unsigned char* p) {
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
            | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
    };
    std::optional<double> gamma;
    // PNG color-space chunks precede IDAT. Skip payloads without buffering the image.
    // libvips still validates and decodes the original file, including its CRCs.
    while (input.read(reinterpret_cast<char*>(header.data()), header.size())) {
        const auto length = u32(header.data());
        const std::string_view type(reinterpret_cast<const char*>(header.data() + 4), 4);
        if (type == "sRGB" || type == "iCCP") {
            return std::nullopt;
        }
        if (type == "IDAT" || type == "IEND") {
            break;
        }
        if (type == "gAMA" && length == 4) {
            std::array<unsigned char, 4> value{};
            if (!input.read(reinterpret_cast<char*>(value.data()), value.size())) {
                break;
            }
            if (const auto encoded = u32(value.data()); encoded != 0) {
                gamma = static_cast<double>(encoded) / 100000.0;
            }
            input.seekg(4, std::ios::cur); // CRC
        } else {
            input.seekg(static_cast<std::streamoff>(length) + 4, std::ios::cur);
        }
    }
    return gamma;
}

Image oriented_image(const std::filesystem::path& source) {
    auto loaded = load(source);
    return operate("Apply image orientation", [&](VipsImage** out) {
        return vips_autorot(loaded.get(), out, nullptr);
    });
}

Image linear_image(VipsImage* oriented, const std::filesystem::path& source) {
    if (vips_image_get_typeof(oriented, VIPS_META_ICC_NAME) == 0) {
        if (const auto gamma = png_gamma(source)) {
            // Expand gray/palette inputs without reducing 16-bit PNG precision.
            const bool sixteen_bit = vips_image_get_format(oriented) == VIPS_FORMAT_USHORT;
            const auto interpretation = sixteen_bit ? VIPS_INTERPRETATION_RGB16 : VIPS_INTERPRETATION_sRGB;
            const double maximum = sixteen_bit ? 65535.0 : 255.0;
            auto rgb = operate("Expand PNG channels", [&](VipsImage** out) {
                return vips_colourspace(oriented, out, interpretation, nullptr);
            });
            auto colors = operate("Read PNG colors", [&](VipsImage** out) {
                return vips_extract_band(rgb.get(), out, 0, "n", 3, nullptr);
            });
            auto normalized = operate("Normalize PNG colors", [&](VipsImage** out) {
                return vips_linear1(colors.get(), out, 1.0 / maximum, 0.0, nullptr);
            });
            auto linear = operate("Decode PNG gamma", [&](VipsImage** out) {
                return vips_pow_const1(normalized.get(), out, 1.0 / *gamma, nullptr);
            });
            Image rgba;
            if (vips_image_hasalpha(rgb.get())) {
                auto alpha = operate("Read PNG alpha", [&](VipsImage** out) {
                    return vips_extract_band(rgb.get(), out, 3, nullptr);
                });
                auto normalized_alpha = operate("Normalize PNG alpha", [&](VipsImage** out) {
                    return vips_linear1(alpha.get(), out, 1.0 / maximum, 0.0, nullptr);
                });
                rgba = operate("Join linear PNG channels", [&](VipsImage** out) {
                    return vips_bandjoin2(linear.get(), normalized_alpha.get(), out, nullptr);
                });
            }
            return operate("Tag linear PNG colors", [&](VipsImage** out) {
                return vips_copy(rgba ? rgba.get() : linear.get(), out,
                    "interpretation", VIPS_INTERPRETATION_scRGB, nullptr);
            });
        }
    }
    auto srgb = operate("Convert image to sRGB", [&](VipsImage** out) {
        if (vips_image_get_typeof(oriented, VIPS_META_ICC_NAME) != 0) {
            return vips_icc_transform(oriented, out, "srgb", "embedded", TRUE, nullptr);
        }
        return vips_colourspace(oriented, out, VIPS_INTERPRETATION_sRGB, nullptr);
    });
    return operate("Convert image to linear light", [&](VipsImage** out) {
        return vips_colourspace(srgb.get(), out, VIPS_INTERPRETATION_scRGB, nullptr);
    });
}

Image rgba_image(VipsImage* linear) {
    auto output = operate("Convert image to sRGB", [&](VipsImage** out) {
        return vips_colourspace(linear, out, VIPS_INTERPRETATION_sRGB, nullptr);
    });
    Image alpha;
    auto* rgba = output.get();
    if (!vips_image_hasalpha(rgba)) {
        alpha = operate("Add opaque alpha", [&](VipsImage** out) {
            return vips_addalpha(output.get(), out, nullptr);
        });
        rgba = alpha.get();
    }
    return operate("Convert pixels to 8-bit channels", [&](VipsImage** out) {
        return vips_cast(rgba, out, VIPS_FORMAT_UCHAR, nullptr);
    });
}

class ExclusiveOutput final {
public:
    explicit ExclusiveOutput(const std::filesystem::path& path) : path_(path) {
        int descriptor = -1;
#ifdef _WIN32
        const auto open_error = _wsopen_s(&descriptor, path.c_str(),
            _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (open_error != 0) {
            throw std::system_error(open_error, std::generic_category(), "Create output");
        }
        file_ = _fdopen(descriptor, "wb");
#else
        descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(), "Create output");
        }
        file_ = ::fdopen(descriptor, "wb");
#endif
        if (file_ == nullptr) {
            const int error = errno;
#ifdef _WIN32
            _close(descriptor);
#else
            ::close(descriptor);
#endif
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
            throw std::system_error(error, std::generic_category(), "Open output stream");
        }
    }

    ~ExclusiveOutput() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
        if (!complete_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    ExclusiveOutput(const ExclusiveOutput&) = delete;
    ExclusiveOutput& operator=(const ExclusiveOutput&) = delete;

    void write(const void* bytes, std::size_t length) {
        if (std::fwrite(bytes, 1, length, file_) != length) {
            throw std::system_error(errno == 0 ? EIO : errno, std::generic_category(), "Write output");
        }
    }

    void finish() {
        auto* file = std::exchange(file_, nullptr);
        if (std::fclose(file) != 0) {
            throw std::system_error(errno == 0 ? EIO : errno, std::generic_category(), "Close output");
        }
        complete_ = true;
    }

private:
    std::filesystem::path path_;
    std::FILE* file_ = nullptr;
    bool complete_ = false;
};

struct MemoryDeleter {
    void operator()(void* pointer) const noexcept { g_free(pointer); }
};

} // namespace

Runtime::Runtime(const char* program_name, int threads) {
    if (threads < 1 || threads > 1024) {
        throw std::invalid_argument("Thread count must be between 1 and 1024.");
    }
    if (vips_init(program_name) != 0) {
        fail_vips("Initialize libvips");
    }
    vips_concurrency_set(threads);
    // Keep lazy image graphs alive only while the command owns them.
    vips_cache_set_max(0);
}

Runtime::~Runtime() {
    vips_shutdown();
}

void ImageDeleter::operator()(VipsImage* image) const noexcept {
    if (image != nullptr) {
        g_object_unref(image);
    }
}

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool is_raster_loader(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::string_view prefix = "vipsforeignload";
    if (lower.starts_with(prefix)) {
        lower.erase(0, prefix.size());
    }
    constexpr std::array<std::string_view, 15> codecs{
        "jpeg", "png", "webp", "tiff", "gif", "nsgif", "heif", "jxl",
        "ppm", "rad", "jp2k", "bmp", "fits", "nifti", "vips"
    };
    return std::any_of(codecs.begin(), codecs.end(),
        [&](std::string_view codec) { return lower.starts_with(codec); });
}

void require_new_output(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("Output path must not be empty.");
    }
    const auto status = std::filesystem::symlink_status(path);
    if (std::filesystem::exists(status) || std::filesystem::is_symlink(status)) {
        throw std::runtime_error("Output already exists: " + path_to_utf8(path));
    }
}

bool paths_equal(const std::filesystem::path& first, const std::filesystem::path& second) {
    const auto normalized_first = std::filesystem::weakly_canonical(first);
    const auto normalized_second = std::filesystem::weakly_canonical(second);
#ifdef _WIN32
    return CompareStringOrdinal(normalized_first.c_str(), -1,
        normalized_second.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return normalized_first == normalized_second;
#endif
}

void validate(const std::filesystem::path& source) {
    auto image = load(source);
    double average = 0;
    // A streaming sink forces decoding without retaining a full managed-style pixel buffer.
    if (vips_avg(image.get(), &average, nullptr) != 0) {
        fail_vips("Decode raster image");
    }
}

Image convert(const std::filesystem::path& source) {
    auto oriented = oriented_image(source);
    auto linear = linear_image(oriented.get(), source);
    return rgba_image(linear.get());
}

Image resize(const std::filesystem::path& source, int width, int height, bool opaque) {
    if (width < 1 || width > 65535 || height < 1 || height > 65535) {
        throw std::invalid_argument("Output dimensions must be between 1 and 65535.");
    }
    auto oriented = oriented_image(source);
    auto linear = linear_image(oriented.get(), source);
    const bool has_alpha = vips_image_hasalpha(linear.get()) != 0;
    Image adjusted;
    auto* filter_input = linear.get();
    if (has_alpha) {
        if (opaque) {
            const double black[]{0, 0, 0};
            auto background = background_array(black, 3);
            adjusted = operate("Composite image over black", [&](VipsImage** out) {
                return vips_flatten(linear.get(), out, "background", background.get(),
                    "max_alpha", 1.0, nullptr);
            });
        } else {
            adjusted = operate("Premultiply image alpha", [&](VipsImage** out) {
                return vips_premultiply(linear.get(), out, "max_alpha", 1.0, nullptr);
            });
        }
        filter_input = adjusted.get();
    }
    auto resized = operate("Resize image", [&](VipsImage** out) {
        return vips_resize(filter_input, out,
            static_cast<double>(width) / vips_image_get_width(filter_input),
            "kernel", VIPS_KERNEL_LANCZOS3,
            "vscale", static_cast<double>(height) / vips_image_get_height(filter_input), nullptr);
    });
    Image straightened;
    auto* straight = resized.get();
    if (!opaque && has_alpha) {
        // libvips uses an absolute 0.01 threshold for floating alpha. Scale only
        // alpha to byte-scale units so valid 1/255 and 2/255 samples keep colors.
        // A power of two avoids rounding alpha during the scale/unscale pair.
        const double scales[]{1, 1, 1, 256};
        const double zero[]{0, 0, 0, 0};
        auto scaled = operate("Scale alpha for unpremultiplication", [&](VipsImage** out) {
            return vips_linear(resized.get(), out, scales, zero, 4, nullptr);
        });
        auto unpremultiplied = operate("Unpremultiply image alpha", [&](VipsImage** out) {
            return vips_unpremultiply(scaled.get(), out, "max_alpha", 256.0, nullptr);
        });
        const double inverse[]{1, 1, 1, 1.0 / 256.0};
        straightened = operate("Restore normalized alpha", [&](VipsImage** out) {
            return vips_linear(unpremultiplied.get(), out, inverse, zero, 4, nullptr);
        });
        straight = straightened.get();
    }
    auto rgba = rgba_image(straight);
    if (vips_image_get_width(rgba.get()) != width || vips_image_get_height(rgba.get()) != height) {
        throw std::runtime_error("Image resize produced unexpected dimensions.");
    }
    return rgba;
}

Image offset_background(VipsImage* image, int offset) {
    if (image == nullptr) {
        throw std::invalid_argument("Background image must not be null.");
    }
    const int width = vips_image_get_width(image);
    const int height = vips_image_get_height(image);
    const auto wide_offset = static_cast<std::int64_t>(offset);
    const double black[]{0, 0, 0, 255};
    if (wide_offset >= height || wide_offset <= -static_cast<std::int64_t>(height)) {
        return own_image(vips_image_new_from_image(image, black, 4), "Create black background");
    }
    if (offset == 0) {
        return operate("Copy background", [&](VipsImage** out) {
            return vips_copy(image, out, nullptr);
        });
    }
    const int shift = offset < 0 ? -offset : offset;
    auto cropped = operate("Crop background", [&](VipsImage** out) {
        return vips_crop(image, out, 0, offset > 0 ? shift : 0, width, height - shift, nullptr);
    });
    auto background = background_array(black, 4);
    return operate("Offset background", [&](VipsImage** out) {
        return vips_embed(cropped.get(), out, 0, offset > 0 ? 0 : shift, width, height,
            "extend", VIPS_EXTEND_BACKGROUND, "background", background.get(), nullptr);
    });
}

Image effect_atlas(const std::array<std::optional<std::filesystem::path>, 4>& paths) {
    std::array<Image, 4> images;
    std::array<VipsImage*, 4> raw{};
    for (std::size_t slot = 0; slot < images.size(); ++slot) {
        images[slot] = paths[slot].has_value()
            ? resize(*paths[slot], 256, 256, false)
            : operate("Create transparent effect", [](VipsImage** out) {
                return vips_black(out, 256, 256, "bands", 4, nullptr);
            });
        raw[slot] = images[slot].get();
    }
    return operate("Join effect atlas", [&](VipsImage** out) {
        return vips_arrayjoin(raw.data(), out, static_cast<int>(raw.size()), "across", 2, nullptr);
    });
}

void write_tga(VipsImage* image, const std::filesystem::path& destination) {
    if (image == nullptr || vips_image_get_bands(image) != 4) {
        throw std::invalid_argument("TGA output requires a four-channel image.");
    }
    const int width = vips_image_get_width(image);
    const int height = vips_image_get_height(image);
    if (width < 1 || height < 1 || width > 65535 || height > 65535) {
        throw std::invalid_argument("TGA output requires 16-bit positive dimensions.");
    }
    require_new_output(destination);
    // Recombine once so the lazy decode/resize graph has a single input branch.
    const double channels[]{
        0, 0, 1, 0,
        0, 1, 0, 0,
        1, 0, 0, 0,
        0, 0, 0, 1
    };
    auto matrix = own_image(vips_image_new_matrix_from_array(4, 4, channels, 16), "Create channel matrix");
    auto reordered = operate("Convert RGBA to BGRA", [&](VipsImage** out) {
        return vips_recomb(image, out, matrix.get(), nullptr);
    });
    auto bgra = operate("Convert TGA channel depth", [&](VipsImage** out) {
        return vips_cast(reordered.get(), out, VIPS_FORMAT_UCHAR, nullptr);
    });
    std::size_t length = 0;
    std::unique_ptr<void, MemoryDeleter> pixels(vips_image_write_to_memory(bgra.get(), &length));
    if (!pixels) {
        fail_vips("Evaluate TGA pixels");
    }
    const auto expected = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4;
    if (expected > std::numeric_limits<std::size_t>::max() || length != expected) {
        throw std::runtime_error("Unexpected TGA pixel buffer length.");
    }
    std::array<unsigned char, 18> header{};
    header[2] = 2;
    header[12] = static_cast<unsigned char>(width & 0xff);
    header[13] = static_cast<unsigned char>((width >> 8) & 0xff);
    header[14] = static_cast<unsigned char>(height & 0xff);
    header[15] = static_cast<unsigned char>((height >> 8) & 0xff);
    header[16] = 32;
    header[17] = 0x28;
    ExclusiveOutput output(destination);
    output.write(header.data(), header.size());
    output.write(pixels.get(), length);
    output.finish();
}

} // namespace image_tools
