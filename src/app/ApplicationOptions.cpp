#include "app/ApplicationOptions.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <system_error>
#include <utility>

namespace pci {
namespace {

bool hasGlobMetacharacters(const std::filesystem::path::string_type &text)
{
    using NativeChar = std::filesystem::path::value_type;
    const std::array metacharacters{
        static_cast<NativeChar>('*'),
        static_cast<NativeChar>('?'),
        static_cast<NativeChar>('['),
    };
    return text.find_first_of(
               metacharacters.data(), 0, metacharacters.size()) !=
           std::filesystem::path::string_type::npos;
}

// Matches `text` (a single path component) against a glob `pattern` at
// pattern[start] == '['. Returns {matched, indexAfterClass}; when the class is
// malformed (no closing ']'), indexAfterClass == start so the caller treats
// '[' as a literal character.
template <typename Character>
std::pair<bool, std::size_t>
matchCharClass(const std::basic_string_view<Character> pattern,
               const std::size_t start,
               const Character ch)
{
    std::size_t i = start + 1;
    bool negate = false;
    if (i < pattern.size() && (pattern[i] == static_cast<Character>('!') ||
                               pattern[i] == static_cast<Character>('^'))) {
        negate = true;
        ++i;
    }
    bool matched = false;
    for (bool first = true; i < pattern.size(); first = false) {
        const Character c = pattern[i];
        if (c == static_cast<Character>(']') && !first) {
            return {matched != negate, i + 1};
        }
        if (i + 2 < pattern.size() &&
            pattern[i + 1] == static_cast<Character>('-') &&
            pattern[i + 2] != static_cast<Character>(']')) {
            if (ch >= c && ch <= pattern[i + 2]) {
                matched = true;
            }
            i += 3;
        } else {
            if (ch == c) {
                matched = true;
            }
            ++i;
        }
    }
    return {false, start}; // no closing bracket
}

template <typename Character>
bool matchGlob(const std::basic_string_view<Character> pattern,
               const std::basic_string_view<Character> text)
{
    std::size_t pi = 0;
    std::size_t ti = 0;
    while (pi < pattern.size()) {
        const Character pc = pattern[pi];
        if (pc == static_cast<Character>('*')) {
            while (pi < pattern.size() &&
                   pattern[pi] == static_cast<Character>('*')) {
                ++pi;
            }
            if (pi == pattern.size()) {
                return true;
            }
            for (std::size_t k = ti; k <= text.size(); ++k) {
                if (matchGlob(pattern.substr(pi), text.substr(k))) {
                    return true;
                }
            }
            return false;
        }
        if (ti >= text.size()) {
            return false;
        }
        if (pc == static_cast<Character>('?')) {
            ++pi;
            ++ti;
        } else if (pc == static_cast<Character>('[')) {
            const auto [ok, next] = matchCharClass(pattern, pi, text[ti]);
            if (next == pi) {
                if (text[ti] != static_cast<Character>('[')) {
                    return false;
                }
                ++pi;
                ++ti;
            } else if (ok) {
                pi = next;
                ++ti;
            } else {
                return false;
            }
        } else {
            if (text[ti] != pc) {
                return false;
            }
            ++pi;
            ++ti;
        }
    }
    return ti == text.size();
}

std::vector<std::filesystem::path>
expandGlob(const std::filesystem::path &pattern)
{
    const std::filesystem::path parent = pattern.parent_path();
    const std::filesystem::path::string_type name = pattern.filename().native();
    const std::filesystem::path scanDir =
        parent.empty() ? std::filesystem::path(".") : parent;

    std::vector<std::filesystem::path> matches;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(scanDir, ec), end;
         it != end && !ec;
         it.increment(ec)) {
        if (it->is_directory(ec)) {
            continue;
        }
        const std::filesystem::path::string_type filename =
            it->path().filename().native();
        if (matchGlob(
                std::basic_string_view<std::filesystem::path::value_type>(name),
                std::basic_string_view<std::filesystem::path::value_type>(
                    filename))) {
            matches.push_back(parent.empty() ? std::filesystem::path(filename)
                                             : parent / filename);
        }
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

} // namespace

std::optional<GraphicsApi>
parseGraphicsApi(const std::string_view text) noexcept
{
    if (text == "auto") {
        return GraphicsApi::Auto;
    }
    if (text == "metal") {
        return GraphicsApi::Metal;
    }
    if (text == "vulkan") {
        return GraphicsApi::Vulkan;
    }
    if (text == "d3d11") {
        return GraphicsApi::Direct3D11;
    }
    if (text == "d3d12") {
        return GraphicsApi::Direct3D12;
    }
    if (text == "opengl") {
        return GraphicsApi::OpenGL;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parsePointCount(const std::string_view text)
{
    std::uint64_t value = 0;
    const auto *begin = text.data();
    const auto *end = begin + text.size();
    const auto [next, error] = std::from_chars(begin, end, value);

    if (error != std::errc{} || next != end || value == 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<MemoryBudgetOption>
parseMemoryBudgetOption(const std::string_view text) noexcept
{
    if (text == "auto") {
        return MemoryBudgetOption{.automatic = true};
    }
    const std::optional<std::uint64_t> value = parsePointCount(text);
    if (!value) {
        return std::nullopt;
    }
    return MemoryBudgetOption{
        .automatic = false,
        .mebibytes = *value,
    };
}

std::vector<std::filesystem::path>
expandPathArguments(const std::vector<std::filesystem::path> &arguments)
{
    std::vector<std::filesystem::path> result;
    std::set<std::filesystem::path> seen;
    const auto add = [&](std::filesystem::path path) {
        if (seen.insert(path).second) {
            result.push_back(std::move(path));
        }
    };

    for (const std::filesystem::path &argument : arguments) {
        if (!hasGlobMetacharacters(argument.native())) {
            add(argument);
            continue;
        }
        for (std::filesystem::path &match : expandGlob(argument)) {
            add(std::move(match));
        }
    }
    return result;
}

} // namespace pci
