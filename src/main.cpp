#include "raster_pipeline.h"

#include <array>
#include <charconv>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#ifndef IMAGE_TOOLS_VERSION
#define IMAGE_TOOLS_VERSION "1.0.0"
#endif

namespace {

class UsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string command;
    std::map<std::string, std::string, std::less<>> values;
    int threads = 1;
    bool opaque = false;
    bool help = false;
    bool version = false;
};

void help() {
    std::cout <<
        "image-tools " IMAGE_TOOLS_VERSION "\n"
        "Usage:\n"
        "  image-tools validate --input PATH [--threads N]\n"
        "  image-tools resize --input PATH --output PATH --width N --height N [--opaque] [--threads N]\n"
        "  image-tools stage --background PATH --background-output PATH --effects-output PATH\n"
        "      [--offset N] [--effect0 PATH] [--effect1 PATH] [--effect2 PATH] [--effect3 PATH] [--threads N]\n"
        "  image-tools --version\n"
        "Outputs are new, uncompressed 32-bit BGRA TGA files; existing files are rejected.\n"
        "Dimensions: 1..65535. Threads: 1..1024 (default 1). Stage offset defaults to 160.\n";
}

int integer(std::string_view value, std::string_view name, int minimum, int maximum) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || result < minimum || result > maximum) {
        throw UsageError("Invalid integer for " + std::string(name) + ": " + std::string(value));
    }
    return result;
}

Options parse(const std::vector<std::string>& args) {
    Options result;
    bool threads_seen = false;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& argument = args[index];
        if (argument == "--help" || argument == "-h") {
            result.help = true;
        } else if (argument == "--version") {
            result.version = true;
        } else if (argument == "--opaque") {
            if (result.opaque) {
                throw UsageError("Duplicate option: --opaque");
            }
            result.opaque = true;
        } else if (argument.starts_with("--")) {
            if (index + 1 >= args.size()) {
                throw UsageError("Missing value for " + argument);
            }
            const auto& value = args[++index];
            if (argument == "--threads") {
                if (threads_seen) {
                    throw UsageError("Duplicate option: --threads");
                }
                threads_seen = true;
                result.threads = integer(value, argument, 1, 1024);
            } else if (!result.values.emplace(argument, value).second) {
                throw UsageError("Duplicate option: " + argument);
            }
        } else if (result.command.empty()) {
            result.command = argument;
        } else {
            throw UsageError("Unexpected argument: " + argument);
        }
    }
    return result;
}

const std::string& required(const Options& options, std::string_view key) {
    const auto found = options.values.find(key);
    if (found == options.values.end() || found->second.empty()) {
        throw UsageError("Required option: " + std::string(key));
    }
    return found->second;
}

void allowed(const Options& options, std::initializer_list<std::string_view> keys) {
    for (const auto& [key, value] : options.values) {
        (void)value;
        bool found = false;
        for (const auto candidate : keys) {
            found = found || key == candidate;
        }
        if (!found) {
            throw UsageError("Unknown option for " + options.command + ": " + key);
        }
    }
    if (options.opaque && options.command != "resize") {
        throw UsageError("--opaque is only supported by resize.");
    }
}

int run(const std::vector<std::string>& args) {
    try {
        const auto options = parse(args);
        if (options.version) {
            std::cout << "image-tools " IMAGE_TOOLS_VERSION "\n";
            return 0;
        }
        if (options.help) {
            help();
            return 0;
        }
        if (options.command == "validate") {
            allowed(options, {"--input"});
            const auto input = image_tools::path_from_utf8(required(options, "--input"));
            image_tools::Runtime runtime("image-tools", options.threads);
            image_tools::validate(input);
        } else if (options.command == "resize") {
            allowed(options, {"--input", "--output", "--width", "--height"});
            const auto input = image_tools::path_from_utf8(required(options, "--input"));
            const auto output = image_tools::path_from_utf8(required(options, "--output"));
            const int width = integer(required(options, "--width"), "--width", 1, 65535);
            const int height = integer(required(options, "--height"), "--height", 1, 65535);
            image_tools::require_new_output(output);
            image_tools::Runtime runtime("image-tools", options.threads);
            auto image = image_tools::resize(input, width, height, options.opaque);
            image_tools::write_tga(image.get(), output);
        } else if (options.command == "stage") {
            allowed(options, {"--background", "--background-output", "--effects-output", "--offset",
                "--effect0", "--effect1", "--effect2", "--effect3"});
            const auto input = image_tools::path_from_utf8(required(options, "--background"));
            const auto background_output = image_tools::path_from_utf8(required(options, "--background-output"));
            const auto effects_output = image_tools::path_from_utf8(required(options, "--effects-output"));
            image_tools::require_new_output(background_output);
            image_tools::require_new_output(effects_output);
            if (image_tools::paths_equal(background_output, effects_output)) {
                throw UsageError("Background and effects outputs must be different files.");
            }
            int offset = 160;
            if (const auto found = options.values.find("--offset"); found != options.values.end()) {
                offset = integer(found->second, "--offset", std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max());
            }
            std::array<std::optional<std::filesystem::path>, 4> effects;
            for (std::size_t slot = 0; slot < effects.size(); ++slot) {
                const auto name = "--effect" + std::to_string(slot);
                if (const auto found = options.values.find(name); found != options.values.end()) {
                    effects[slot] = image_tools::path_from_utf8(required(options, name));
                }
            }
            image_tools::Runtime runtime("image-tools", options.threads);
            auto background = image_tools::resize(input, 1920, 1080, true);
            auto shifted = image_tools::offset_background(background.get(), offset);
            auto atlas = image_tools::effect_atlas(effects);
            image_tools::write_tga(shifted.get(), background_output);
            image_tools::write_tga(atlas.get(), effects_output);
        } else {
            throw UsageError(options.command.empty() ? "A command is required." : "Unknown command: " + options.command);
        }
        return 0;
    } catch (const UsageError& error) {
        std::cerr << "image-tools: " << error.what() << "\nUse --help for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "image-tools: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
std::string utf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        throw UsageError("Command line contains invalid Unicode.");
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
        result.data(), length, nullptr, nullptr) == 0) {
        throw UsageError("Unable to decode the command line.");
    }
    result.pop_back();
    return result;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            args.push_back(utf8(argv[index]));
        }
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "image-tools: " << error.what() << '\n';
        return 2;
    }
}
#else
int main(int argc, char* argv[]) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
