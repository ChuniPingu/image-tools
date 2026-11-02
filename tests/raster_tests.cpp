#include "raster_pipeline.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using image_tools::Image;
using Path = std::filesystem::path;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_vips(int status, const char* message) {
    if (status != 0) {
        const std::string detail = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error(std::string(message) + ": " + detail);
    }
}

template<class Action>
void throws(Action action, const char* message) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, message);
}

class Workspace final {
public:
    Workspace() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            root = std::filesystem::temp_directory_path() / image_tools::path_from_utf8(
                "image-tools 透明 tests " + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(root)) {
                return;
            }
        }
        throw std::runtime_error("Unable to create test workspace.");
    }
    ~Workspace() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    Path root;
};

std::vector<unsigned char> read(const Path& path) {
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "Open fixture for reading");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write(const Path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(stream.good(), "Write fixture");
}

Image rgba(const std::vector<unsigned char>& bytes, int width, int height) {
    check(bytes.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
        "RGBA fixture dimensions");
    Image raw(vips_image_new_from_memory_copy(bytes.data(), bytes.size(), width, height, 4, VIPS_FORMAT_UCHAR));
    check(raw != nullptr, "Create RGBA fixture");
    VipsImage* tagged = nullptr;
    check_vips(vips_copy(raw.get(), &tagged, "interpretation", VIPS_INTERPRETATION_sRGB, nullptr), "Tag fixture");
    return Image(tagged);
}

Image solid(std::array<unsigned char, 4> color, int width = 2, int height = 2) {
    std::vector<unsigned char> bytes;
    for (int index = 0; index < width * height; ++index) {
        bytes.insert(bytes.end(), color.begin(), color.end());
    }
    return rgba(bytes, width, height);
}

Path png(const Workspace& workspace, const char* name, VipsImage* image) {
    const auto path = workspace.root / image_tools::path_from_utf8(name);
    const auto filename = image_tools::path_to_utf8(path);
    check_vips(vips_pngsave(image, filename.c_str(), nullptr), "Save PNG fixture");
    return path;
}

std::vector<unsigned char> pixels(VipsImage* image) {
    check(vips_image_get_format(image) == VIPS_FORMAT_UCHAR, "Expect 8-bit image");
    std::size_t length = 0;
    auto* memory = static_cast<unsigned char*>(vips_image_write_to_memory(image, &length));
    check(memory != nullptr, "Evaluate fixture pixels");
    std::vector<unsigned char> result(memory, memory + length);
    g_free(memory);
    return result;
}

std::array<unsigned char, 4> pixel(const std::vector<unsigned char>& bytes, int width, int x, int y) {
    const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
        + static_cast<std::size_t>(x)) * 4;
    check(offset + 4 <= bytes.size(), "Pixel is in bounds");
    return {bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]};
}

void content_validation(const Workspace& workspace) {
    auto image = solid({1, 2, 3, 255});
    const auto source = png(workspace, "图片 with spaces.unknown", image.get());
    image_tools::validate(source);
    auto truncated = read(source);
    truncated.resize(truncated.size() / 2);
    const auto corrupt = workspace.root / "truncated.png";
    write(corrupt, truncated);
    throws([&] { image_tools::validate(corrupt); }, "Truncated image must fail full decode");
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='2'><rect width='2' height='2'/></svg>";
    const auto vector = workspace.root / "vector.png";
    write(vector, {svg.begin(), svg.end()});
    throws([&] { image_tools::validate(vector); }, "Content-based SVG rejection");
}

void gif_loader(const Workspace& workspace) {
    check(image_tools::is_raster_loader("VipsForeignLoadNsgifFile"), "Allow the actual GIF loader");
    check(image_tools::is_raster_loader("VipsForeignLoadGif"), "Allow GIF loader alias");
    check(!image_tools::is_raster_loader("VipsForeignLoadSvg"), "Reject SVG loader");
    check(!image_tools::is_raster_loader("VipsForeignLoadMagickFile"), "Reject unrestricted Magick loader");
    // Complete 1x1 GIF89a: clear, pixel zero, end-of-information LZW codes.
    const std::vector<unsigned char> gif{
        'G','I','F','8','9','a',1,0,1,0,0x80,0,0, 0,0,0, 255,255,255,
        0x2c,0,0,0,0,1,0,1,0,0, 2,2,0x44,0x01,0,0x3b
    };
    const auto source = workspace.root / "raster disguised.dat";
    write(source, gif);
    image_tools::validate(source);
    auto image = image_tools::resize(source, 3, 2, false);
    check(vips_image_get_width(image.get()) == 3 && vips_image_get_height(image.get()) == 2, "Resize GIF");
}

void alpha_and_linear_light(const Workspace& workspace) {
    auto image = solid({255, 0, 0, 128});
    const auto source = png(workspace, "half-alpha.png", image.get());
    auto opaque = image_tools::resize(source, 7, 5, true);
    const auto black_composite = pixel(pixels(opaque.get()), 7, 3, 2);
    check(black_composite[0] >= 180 && black_composite[0] <= 195, "Composite in linear light");
    check(black_composite[1] == 0 && black_composite[2] == 0 && black_composite[3] == 255, "Opaque black composite");
    auto transparent = image_tools::resize(source, 7, 5, false);
    const auto preserved = pixel(pixels(transparent.get()), 7, 3, 2);
    check(preserved[0] >= 250 && preserved[2] == 0 && preserved[3] >= 126 && preserved[3] <= 129,
        "Preserve straight alpha after filtering");
}

void hidden_color_filtering(const Workspace& workspace) {
    auto image = rgba({255, 0, 0, 255, 0, 0, 255, 0}, 2, 1);
    const auto source = png(workspace, "hidden-blue.png", image.get());
    auto resized = image_tools::resize(source, 1, 1, false);
    const auto value = pixel(pixels(resized.get()), 1, 0, 0);
    check(value[0] >= 250 && value[2] == 0 && value[3] > 0 && value[3] < 255,
        "Transparent blue must not bleed into red");
}

void tga_layout_and_output_protection(const Workspace& workspace) {
    const std::vector<unsigned char> input{
        10,20,30,40, 255,128,64,32,
        1,2,3,255, 0,0,0,0
    };
    auto image = rgba(input, 2, 2);
    const auto path = workspace.root / image_tools::path_from_utf8("输出 with spaces.tga");
    image_tools::write_tga(image.get(), path);
    const auto output = read(path);
    check(output.size() == 18 + input.size(), "TGA length");
    std::array<unsigned char, 18> expected_header{};
    expected_header[2] = 2;
    expected_header[12] = 2;
    expected_header[14] = 2;
    expected_header[16] = 32;
    expected_header[17] = 0x28;
    check(std::equal(expected_header.begin(), expected_header.end(), output.begin()), "TGA top-left 32-bit header");
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
        check(output[18 + offset] == input[offset + 2] && output[19 + offset] == input[offset + 1]
            && output[20 + offset] == input[offset] && output[21 + offset] == input[offset + 3],
            "TGA BGRA pixel ordering");
    }
    throws([&] { image_tools::write_tga(image.get(), path); }, "Existing destination must not be overwritten");
    check(read(path) == output, "Existing output remains intact after rejection");
    check(image_tools::paths_equal(path, path.parent_path() / "." / path.filename()), "Normalize output aliases");
}

void offsets() {
    auto image = rgba({10,0,0,255, 20,0,0,255, 30,0,0,255}, 1, 3);
    auto up = image_tools::offset_background(image.get(), 1);
    auto up_pixels = pixels(up.get());
    check(pixel(up_pixels, 1, 0, 0)[0] == 20
        && pixel(up_pixels, 1, 0, 2) == std::array<unsigned char,4>{0,0,0,255}, "Positive offset");
    auto down = image_tools::offset_background(image.get(), -1);
    auto down_pixels = pixels(down.get());
    check(pixel(down_pixels, 1, 0, 2)[0] == 20
        && pixel(down_pixels, 1, 0, 0) == std::array<unsigned char,4>{0,0,0,255}, "Negative offset");
    auto unchanged = image_tools::offset_background(image.get(), 0);
    check(pixels(unchanged.get()) == pixels(image.get()), "Zero offset preserves pixels");
    for (const int offset : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), -3, 3}) {
        auto black = image_tools::offset_background(image.get(), offset);
        check(pixel(pixels(black.get()), 1, 0, 1) == std::array<unsigned char,4>{0,0,0,255},
            "Out-of-bounds and extreme offsets are opaque black");
    }
}

void atlas(const Workspace& workspace) {
    auto red = solid({255, 0, 0, 128});
    auto blue = solid({0, 0, 255, 255});
    const auto red_path = png(workspace, "atlas-red.png", red.get());
    const auto blue_path = png(workspace, "atlas-blue.png", blue.get());
    std::array<std::optional<Path>, 4> slots{red_path, std::nullopt, std::nullopt, blue_path};
    auto image = image_tools::effect_atlas(slots);
    check(vips_image_get_width(image.get()) == 512 && vips_image_get_height(image.get()) == 512, "Atlas dimensions");
    const auto bytes = pixels(image.get());
    const auto first = pixel(bytes, 512, 100, 100);
    const auto fourth = pixel(bytes, 512, 400, 400);
    check(first[0] >= 250 && first[3] >= 126 && first[3] <= 129, "First atlas slot preserves alpha");
    check(pixel(bytes, 512, 400, 100) == std::array<unsigned char,4>{0,0,0,0}, "Second slot transparent");
    check(pixel(bytes, 512, 100, 400) == std::array<unsigned char,4>{0,0,0,0}, "Third slot transparent");
    check(fourth[2] >= 250 && fourth[3] == 255, "Fourth slot placement");
}

void grayscale(const Workspace& workspace) {
    const std::array<std::uint16_t, 4> values{32768,32768,32768,32768};
    Image raw(vips_image_new_from_memory_copy(values.data(), sizeof(values), 2, 2, 1, VIPS_FORMAT_USHORT));
    check(raw != nullptr, "Create 16-bit gray fixture");
    VipsImage* tagged = nullptr;
    check_vips(vips_copy(raw.get(), &tagged, "interpretation", VIPS_INTERPRETATION_GREY16, nullptr), "Tag gray fixture");
    Image image(tagged);
    const auto source = png(workspace, "gray16.png", image.get());
    auto resized = image_tools::resize(source, 4, 4, true);
    const auto value = pixel(pixels(resized.get()), 4, 1, 1);
    check(value[0] >= 125 && value[0] <= 131 && value[0] == value[1] && value[1] == value[2]
        && value[3] == 255, "Scale 16-bit gray into sRGB");
}

void first_page(const Workspace& workspace) {
    auto image = rgba({
        255,0,0,255, 255,0,0,255,
        255,0,0,255, 255,0,0,255,
        0,0,255,255, 0,0,255,255,
        0,0,255,255, 0,0,255,255
    }, 2, 4);
    const auto source = workspace.root / "pages.tif";
    const auto name = image_tools::path_to_utf8(source);
    check_vips(vips_tiffsave(image.get(), name.c_str(), "page_height", 2, nullptr), "Save multipage TIFF");
    auto resized = image_tools::resize(source, 8, 8, true);
    const auto last = pixel(pixels(resized.get()), 8, 4, 7);
    check(last[0] >= 250 && last[2] == 0, "Read only first TIFF page");
}


void literal_input_paths(const Workspace& workspace) {
    auto pages = rgba({
        255,0,0,255, 255,0,0,255,
        255,0,0,255, 255,0,0,255,
        0,0,255,255, 0,0,255,255,
        0,0,255,255, 0,0,255,255
    }, 2, 4);
    const auto base = workspace.root / "literal-base.tif";
    const auto base_name = image_tools::path_to_utf8(base);
    check_vips(vips_tiffsave(pages.get(), base_name.c_str(), "page_height", 2, nullptr),
        "Save literal-path fixture");
    const auto alias = image_tools::path_from_utf8(base_name + "[n=-1]");
    check(!std::filesystem::exists(alias), "Option-like input path does not exist");
    throws([&] { image_tools::validate(alias); }, "Missing literal path must not become loader options");
    throws([&] {
        auto result = image_tools::resize(alias, 4, 4, true);
        static_cast<void>(pixels(result.get()));
    }, "Resize must reject a missing literal path");

    for (const auto* name : {"actual[n=-1].tif", "真实 image.tif[n=-1]", "empty-options.tif[]"}) {
        const auto literal = workspace.root / image_tools::path_from_utf8(name);
        std::filesystem::copy_file(base, literal);
        image_tools::validate(literal);
        auto resized = image_tools::resize(literal, 4, 4, true);
        const auto last = pixel(pixels(resized.get()), 4, 2, 3);
        check(last[0] >= 250 && last[2] == 0, "Bracketed literal path keeps first-page semantics");
    }

    auto green = solid({0, 255, 0, 255});
    const auto green_path = png(workspace, "literal-green.png", green.get());
    std::filesystem::copy_file(green_path, alias);
    image_tools::validate(alias);
    auto exact_file = image_tools::resize(alias, 4, 4, true);
    const auto value = pixel(pixels(exact_file.get()), 4, 2, 2);
    check(value[0] == 0 && value[1] >= 250 && value[2] == 0,
        "Literal file content wins over an existing base filename");
}

void orientation(const Workspace& workspace) {
    auto image = rgba({255,0,0,255, 0,0,255,255}, 2, 1);
    vips_image_set_int(image.get(), VIPS_META_ORIENTATION, 6);
    const auto source = workspace.root / "oriented.tif";
    const auto name = image_tools::path_to_utf8(source);
    check_vips(vips_tiffsave(image.get(), name.c_str(), nullptr), "Save oriented TIFF");
    auto resized = image_tools::resize(source, 1, 2, true);
    const auto bytes = pixels(resized.get());
    check(pixel(bytes, 1, 0, 0)[0] >= 250 && pixel(bytes, 1, 0, 1)[2] >= 250, "Apply EXIF orientation before resizing");
}

void embedded_profile(const Workspace& workspace) {
    auto image = solid({10, 100, 220, 255});
    VipsBlob* profile = nullptr;
    check_vips(vips_profile_load("srgb", &profile, nullptr), "Load built-in sRGB profile");
    check(profile != nullptr, "sRGB profile exists");
    std::size_t length = 0;
    const auto* bytes = vips_blob_get(profile, &length);
    vips_image_set_blob_copy(image.get(), VIPS_META_ICC_NAME, bytes, length);
    vips_area_unref(reinterpret_cast<VipsArea*>(profile));
    const auto source = png(workspace, "embedded-profile.png", image.get());
    const auto name = image_tools::path_to_utf8(source);
    Image readback(vips_image_new_from_file(name.c_str(), nullptr));
    check(readback != nullptr && vips_image_get_typeof(readback.get(), VIPS_META_ICC_NAME) != 0, "Fixture embeds ICC profile");
    auto resized = image_tools::resize(source, 3, 3, true);
    const auto value = pixel(pixels(resized.get()), 3, 1, 1);
    check(value[0] >= 8 && value[0] <= 12 && value[1] >= 98 && value[1] <= 102
        && value[2] >= 218 && value[2] <= 222, "Convert embedded ICC profile into sRGB");
}

void set_png_gamma(const Path& path, std::uint32_t gamma) {
    auto bytes = read(path);
    const auto append_u32 = [](std::vector<unsigned char>& target, std::uint32_t value) {
        for (const int shift : {24, 16, 8, 0}) {
            target.push_back(static_cast<unsigned char>(value >> shift));
        }
    };
    std::vector<unsigned char> chunk;
    append_u32(chunk, 4);
    chunk.insert(chunk.end(), {'g', 'A', 'M', 'A'});
    append_u32(chunk, gamma);
    std::uint32_t crc = 0xffffffff;
    for (std::size_t i = 4; i < chunk.size(); ++i) {
        crc ^= chunk[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
        }
    }
    append_u32(chunk, crc ^ 0xffffffff);
    // libvips' fixture writer emits no gAMA or sRGB chunk. Insert before IDAT.
    bytes.insert(bytes.begin() + 33, chunk.begin(), chunk.end());
    write(path, bytes);
}

void standalone_png_gamma(const Workspace& workspace) {
    auto image = solid({128, 128, 128, 128});
    for (const auto [gamma, expected] : {std::pair{100000u, 188}, std::pair{60000u, 153}}) {
        const auto source = png(workspace, "gamma.png", image.get());
        set_png_gamma(source, gamma);
        auto converted = image_tools::convert(source);
        auto resized = image_tools::resize(source, 7, 5, false);
        for (auto* result : {converted.get(), resized.get()}) {
            const auto value = pixel(pixels(result), vips_image_get_width(result), 0, 0);
            check(value[0] >= expected - 1 && value[0] <= expected + 1
                && value[0] == value[1] && value[1] == value[2] && value[3] == 128,
                "Decode standalone PNG gamma without applying it to alpha");
        }
    }

    const std::array<std::uint16_t, 4> values{32768,32768,32768,32768};
    Image raw(vips_image_new_from_memory_copy(values.data(), sizeof(values), 2, 2, 1, VIPS_FORMAT_USHORT));
    VipsImage* tagged = nullptr;
    check_vips(vips_copy(raw.get(), &tagged, "interpretation", VIPS_INTERPRETATION_GREY16, nullptr), "Tag linear gray");
    Image gray(tagged);
    const auto source = png(workspace, "gamma-gray16.png", gray.get());
    set_png_gamma(source, 100000);
    auto converted = image_tools::convert(source);
    const auto value = pixel(pixels(converted.get()), 2, 0, 0);
    check(value == std::array<unsigned char,4>{188,188,188,255}, "Decode 16-bit gray gamma");
}

void low_alpha(const Workspace& workspace) {
    for (const auto alpha : std::array<unsigned char, 5>{1, 2, 3, 128, 255}) {
        auto image = solid({255, 0, 0, alpha});
        const auto source = png(workspace, "low-alpha.png", image.get());
        for (const int size : {2, 7}) {
            auto result = image_tools::resize(source, size, size, false);
            const auto value = pixel(pixels(result.get()), size, 0, 0);
            check(value == std::array<unsigned char,4>{255,0,0,alpha},
                "Preserve nonzero alpha colors with and without resizing");
        }
    }
}

void raster_conversion(const Workspace& workspace) {
    auto image = rgba({255,0,0,1, 0,0,255,2}, 2, 1);
    vips_image_set_int(image.get(), VIPS_META_ORIENTATION, 6);
    const auto source = workspace.root / "convert-oriented.tif";
    const auto name = image_tools::path_to_utf8(source);
    check_vips(vips_tiffsave(image.get(), name.c_str(), nullptr), "Save conversion fixture");
    auto result = image_tools::convert(source);
    check(vips_image_get_width(result.get()) == 1 && vips_image_get_height(result.get()) == 2,
        "Conversion preserves oriented dimensions");
    check(pixels(result.get()) == std::vector<unsigned char>{255,0,0,1, 0,0,255,2},
        "Conversion preserves straight alpha");
}

} // namespace

int main() {
    try {
        image_tools::Runtime runtime("image-tools-tests", 1);
        Workspace workspace;
        const std::vector<std::pair<const char*, std::function<void()>>> tests{
            {"content validation and Unicode paths", [&] { content_validation(workspace); }},
            {"GIF content loader", [&] { gif_loader(workspace); }},
            {"linear alpha compositing", [&] { alpha_and_linear_light(workspace); }},
            {"premultiplied filtering", [&] { hidden_color_filtering(workspace); }},
            {"TGA layout and output protection", [&] { tga_layout_and_output_protection(workspace); }},
            {"background offsets", offsets},
            {"effect atlas", [&] { atlas(workspace); }},
            {"16-bit grayscale", [&] { grayscale(workspace); }},
            {"first TIFF page", [&] { first_page(workspace); }},
            {"literal input paths", [&] { literal_input_paths(workspace); }},
            {"EXIF orientation", [&] { orientation(workspace); }},
            {"embedded ICC profile", [&] { embedded_profile(workspace); }},
            {"standalone PNG gamma", [&] { standalone_png_gamma(workspace); }},
            {"low alpha colors", [&] { low_alpha(workspace); }},
            {"raster conversion", [&] { raster_conversion(workspace); }}
        };
        for (const auto& [name, test] : tests) {
            try {
                test();
                std::cout << "PASS " << name << '\n';
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(name) + ": " + error.what());
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
