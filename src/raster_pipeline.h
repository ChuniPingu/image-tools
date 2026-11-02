#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <vips/vips.h>

namespace image_tools {

class Runtime final {
public:
    explicit Runtime(const char* program_name, int threads = 1);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};

struct ImageDeleter {
    void operator()(VipsImage* image) const noexcept;
};

using Image = std::unique_ptr<VipsImage, ImageDeleter>;

std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);
bool is_raster_loader(std::string_view name);
void require_new_output(const std::filesystem::path& path);
bool paths_equal(const std::filesystem::path& first, const std::filesystem::path& second);

void validate(const std::filesystem::path& source);
Image convert(const std::filesystem::path& source);
Image resize(const std::filesystem::path& source, int width, int height, bool opaque);
Image offset_background(VipsImage* image, int offset);
Image effect_atlas(const std::array<std::optional<std::filesystem::path>, 4>& paths);
void write_tga(VipsImage* image, const std::filesystem::path& destination);

} // namespace image_tools
