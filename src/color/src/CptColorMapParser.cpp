#include <pci/color/CptColorMapParser.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace pci {
namespace {

enum class ColorModel {
    Rgb,
    Hsv,
    Cmyk,
};

struct RawInterval {
    double minimum = 0.0;
    PointRgba minimumColor;
    double maximum = 0.0;
    PointRgba maximumColor;
};

std::string_view trim(const std::string_view value) noexcept
{
    constexpr std::string_view whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::string uppercase(std::string_view value)
{
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return result;
}

std::vector<std::string_view> splitWhitespace(const std::string_view line)
{
    std::vector<std::string_view> tokens;
    std::size_t offset = 0;
    while (offset < line.size()) {
        offset = line.find_first_not_of(" \t", offset);
        if (offset == std::string_view::npos) {
            break;
        }
        const std::size_t end = line.find_first_of(" \t", offset);
        tokens.push_back(line.substr(offset,
                                     end == std::string_view::npos
                                         ? line.size() - offset
                                         : end - offset));
        if (end == std::string_view::npos) {
            break;
        }
        offset = end;
    }
    return tokens;
}

bool parseNumber(const std::string_view token, double &value) noexcept
{
    try {
        std::istringstream stream{std::string(token)};
        stream.imbue(std::locale::classic());
        stream >> value;
        return stream && stream.peek() == std::char_traits<char>::eof() &&
               std::isfinite(value);
    } catch (...) {
        return false;
    }
}

bool parseHexByte(const std::string_view token,
                  const std::size_t offset,
                  std::uint8_t &value) noexcept
{
    unsigned int component = 0;
    const char *begin = token.data() + offset;
    const char *end = begin + 2;
    const auto result = std::from_chars(begin, end, component, 16);
    if (result.ec != std::errc{} || result.ptr != end ||
        component > std::numeric_limits<std::uint8_t>::max()) {
        return false;
    }
    value = static_cast<std::uint8_t>(component);
    return true;
}

std::vector<std::string_view> split(const std::string_view value,
                                    const char separator)
{
    std::vector<std::string_view> components;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find(separator, begin);
        components.push_back(value.substr(begin,
                                          end == std::string_view::npos
                                              ? value.size() - begin
                                              : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return components;
}

PointRgba hsvToRgb(double hue,
                   const double saturation,
                   const double value,
                   const float alpha) noexcept
{
    hue = std::fmod(hue, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    const double chroma = value * saturation;
    const double section = hue / 60.0;
    const double x = chroma * (1.0 - std::abs(std::fmod(section, 2.0) - 1.0));
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (section < 1.0) {
        red = chroma;
        green = x;
    } else if (section < 2.0) {
        red = x;
        green = chroma;
    } else if (section < 3.0) {
        green = chroma;
        blue = x;
    } else if (section < 4.0) {
        green = x;
        blue = chroma;
    } else if (section < 5.0) {
        red = x;
        blue = chroma;
    } else {
        red = chroma;
        blue = x;
    }
    const double match = value - chroma;
    return {
        .red = static_cast<float>(red + match),
        .green = static_cast<float>(green + match),
        .blue = static_cast<float>(blue + match),
        .alpha = alpha,
    };
}

bool componentsToColor(const std::span<const std::string_view> componentTokens,
                       const ColorModel model,
                       const float alpha,
                       PointRgba &color,
                       std::string &error)
{
    std::array<double, 4> components{};
    const std::size_t expected = model == ColorModel::Cmyk ? 4 : 3;
    if (componentTokens.size() != expected) {
        error = model == ColorModel::Cmyk
                    ? "CMYK colors require four components"
                    : "RGB and HSV colors require three components";
        return false;
    }
    for (std::size_t index = 0; index < expected; ++index) {
        if (!parseNumber(componentTokens[index], components[index])) {
            error = "color component is not a finite number";
            return false;
        }
    }

    switch (model) {
    case ColorModel::Rgb:
        if (std::ranges::any_of(std::span{components}.first<3>(),
                                [](const double component) {
                                    return component < 0.0 || component > 255.0;
                                })) {
            error = "RGB components must be in [0, 255]";
            return false;
        }
        color = {
            .red = static_cast<float>(components[0] / 255.0),
            .green = static_cast<float>(components[1] / 255.0),
            .blue = static_cast<float>(components[2] / 255.0),
            .alpha = alpha,
        };
        return true;
    case ColorModel::Hsv:
        if (components[0] < 0.0 || components[0] > 360.0 ||
            components[1] < 0.0 || components[1] > 1.0 || components[2] < 0.0 ||
            components[2] > 1.0) {
            error = "HSV components require H in [0, 360] and S/V in [0, 1]";
            return false;
        }
        color = hsvToRgb(components[0], components[1], components[2], alpha);
        return true;
    case ColorModel::Cmyk:
        if (std::ranges::any_of(components, [](const double component) {
                return component < 0.0 || component > 100.0;
            })) {
            error = "CMYK components must be in [0, 100]";
            return false;
        }
        {
            const double cyan = components[0] / 100.0;
            const double magenta = components[1] / 100.0;
            const double yellow = components[2] / 100.0;
            const double black = components[3] / 100.0;
            color = {
                .red = static_cast<float>((1.0 - cyan) * (1.0 - black)),
                .green = static_cast<float>((1.0 - magenta) * (1.0 - black)),
                .blue = static_cast<float>((1.0 - yellow) * (1.0 - black)),
                .alpha = alpha,
            };
        }
        return true;
    }
    error = "unknown color model";
    return false;
}

bool parseColor(std::string_view token,
                const ColorModel model,
                PointRgba &color,
                std::string &error)
{
    float alpha = 1.0F;
    const std::size_t transparency = token.rfind('@');
    if (transparency != std::string_view::npos) {
        double percent = 0.0;
        if (!parseNumber(token.substr(transparency + 1), percent) ||
            percent < 0.0 || percent > 100.0) {
            error = "transparency must be in [0, 100]";
            return false;
        }
        alpha = static_cast<float>(1.0 - percent / 100.0);
        token = token.substr(0, transparency);
    }

    if (token.size() == 7 && token.front() == '#') {
        std::array<std::uint8_t, 3> components{};
        if (!parseHexByte(token, 1, components[0]) ||
            !parseHexByte(token, 3, components[1]) ||
            !parseHexByte(token, 5, components[2])) {
            error = "invalid #RRGGBB color";
            return false;
        }
        color = {
            .red = static_cast<float>(components[0]) / 255.0F,
            .green = static_cast<float>(components[1]) / 255.0F,
            .blue = static_cast<float>(components[2]) / 255.0F,
            .alpha = alpha,
        };
        return true;
    }

    if (model == ColorModel::Rgb && token.find('/') == std::string_view::npos) {
        double gray = 0.0;
        if (parseNumber(token, gray)) {
            if (gray < 0.0 || gray > 255.0) {
                error = "grayscale value must be in [0, 255]";
                return false;
            }
            const float normalized = static_cast<float>(gray / 255.0);
            color = {normalized, normalized, normalized, alpha};
            return true;
        }
    }

    const char separator = model == ColorModel::Hsv ? '-' : '/';
    const std::vector<std::string_view> components = split(token, separator);
    return componentsToColor(components, model, alpha, color, error);
}

std::size_t componentCount(const ColorModel model) noexcept
{
    return model == ColorModel::Cmyk ? 4 : 3;
}

bool parseRegularInterval(const std::vector<std::string_view> &tokens,
                          const ColorModel model,
                          RawInterval &interval,
                          std::string &error)
{
    const std::size_t count = componentCount(model);
    const std::size_t separatedCount = 2 + count * 2;
    if (tokens.size() < separatedCount && tokens.size() >= 4) {
        if (parseNumber(tokens[0], interval.minimum) &&
            parseColor(tokens[1], model, interval.minimumColor, error) &&
            parseNumber(tokens[2], interval.maximum)) {
            if (tokens[3] == "-") {
                interval.maximumColor = interval.minimumColor;
                return true;
            }
            if (parseColor(tokens[3], model, interval.maximumColor, error)) {
                return true;
            }
        }
    }

    error.clear();
    if (tokens.size() < separatedCount ||
        !parseNumber(tokens[0], interval.minimum) ||
        !parseNumber(tokens[count + 1], interval.maximum)) {
        error = "expected 'z0 color0 z1 color1' regular CPT interval";
        return false;
    }
    if (!componentsToColor(std::span{tokens}.subspan(1, count),
                           model,
                           1.0F,
                           interval.minimumColor,
                           error) ||
        !componentsToColor(std::span{tokens}.subspan(count + 2, count),
                           model,
                           1.0F,
                           interval.maximumColor,
                           error)) {
        return false;
    }
    return true;
}

CptColorMapParseResult failure(const std::size_t line, std::string message)
{
    return std::unexpected(CptColorMapParseError{
        .line = line,
        .message = std::move(message),
    });
}

bool sameBoundary(const double left, const double right) noexcept
{
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <=
           std::numeric_limits<double>::epsilon() * scale * 16.0;
}

} // namespace

CptColorMapParseResult parseCptColorMap(const std::string_view contents,
                                        const std::string_view fallbackName)
{
    ColorModel colorModel = ColorModel::Rgb;
    std::string name(trim(fallbackName));
    std::string description;
    std::vector<RawInterval> intervals;

    std::size_t lineNumber = 0;
    std::size_t offset = 0;
    while (offset <= contents.size()) {
        ++lineNumber;
        const std::size_t newline = contents.find('\n', offset);
        std::string_view line = trim(contents.substr(
            offset,
            newline == std::string_view::npos ? contents.size() - offset
                                              : newline - offset));
        if (newline == std::string_view::npos) {
            offset = contents.size() + 1;
        } else {
            offset = newline + 1;
        }
        if (line.empty()) {
            continue;
        }

        if (line.front() == '#') {
            const std::string_view directive = trim(line.substr(1));
            const std::size_t equals = directive.find('=');
            const std::string key =
                uppercase(trim(directive.substr(0, equals)));
            if (key == "COLOR_MODEL") {
                if (equals == std::string_view::npos) {
                    return failure(lineNumber,
                                   "COLOR_MODEL requires '=' and a model name");
                }
                std::string model =
                    uppercase(trim(directive.substr(equals + 1)));
                if (!model.empty() && model.front() == '+') {
                    return failure(
                        lineNumber,
                        "color-space interpolation models are not supported");
                }
                if (model == "RGB") {
                    colorModel = ColorModel::Rgb;
                } else if (model == "HSV") {
                    colorModel = ColorModel::Hsv;
                } else if (model == "CMYK") {
                    colorModel = ColorModel::Cmyk;
                } else {
                    return failure(lineNumber,
                                   "unsupported COLOR_MODEL '" + model + "'");
                }
            } else if (key == "PCINSPECTOR_NAME") {
                if (equals == std::string_view::npos ||
                    trim(directive.substr(equals + 1)).empty()) {
                    return failure(
                        lineNumber,
                        "PCINSPECTOR_NAME requires a non-empty value");
                }
                name = std::string(trim(directive.substr(equals + 1)));
            } else if (key == "PCINSPECTOR_DESCRIPTION") {
                if (equals == std::string_view::npos ||
                    trim(directive.substr(equals + 1)).empty()) {
                    return failure(
                        lineNumber,
                        "PCINSPECTOR_DESCRIPTION requires a non-empty value");
                }
                description = std::string(trim(directive.substr(equals + 1)));
            } else if (key == "CYCLIC") {
                return failure(lineNumber,
                               "cyclic CPT maps are not supported by clamped "
                               "scalar ranges");
            }
            continue;
        }

        const std::size_t label = line.find(';');
        line = trim(line.substr(0, label));
        const std::vector<std::string_view> tokens = splitWhitespace(line);
        if (tokens.empty()) {
            continue;
        }

        if (tokens.front().size() == 1 &&
            (tokens.front() == "B" || tokens.front() == "F" ||
             tokens.front() == "N")) {
            if (tokens.size() < 2) {
                return failure(lineNumber,
                               "B, F, and N records require a color");
            }
            PointRgba ignored;
            std::string colorError;
            const std::size_t count = componentCount(colorModel);
            bool valid = false;
            if (tokens.size() == count + 1) {
                valid = componentsToColor(std::span{tokens}.subspan(1, count),
                                          colorModel,
                                          1.0F,
                                          ignored,
                                          colorError);
            } else {
                valid = parseColor(tokens[1], colorModel, ignored, colorError);
            }
            if (!valid) {
                return failure(lineNumber,
                               "invalid special color: " + colorError);
            }
            continue;
        }

        RawInterval interval;
        std::string intervalError;
        if (!parseRegularInterval(
                tokens, colorModel, interval, intervalError)) {
            return failure(lineNumber, std::move(intervalError));
        }
        if (!(interval.minimum < interval.maximum)) {
            return failure(
                lineNumber,
                "CPT interval maximum must be greater than its minimum");
        }
        if (!intervals.empty() &&
            !sameBoundary(intervals.back().maximum, interval.minimum)) {
            return failure(
                lineNumber,
                "regular CPT intervals must be contiguous and ordered");
        }
        intervals.push_back(interval);
    }

    if (intervals.empty()) {
        return failure(0, "CPT contains no regular color intervals");
    }
    if (name.empty()) {
        return failure(0, "CPT color map has no display name");
    }

    const double minimum = intervals.front().minimum;
    const double maximum = intervals.back().maximum;
    const double extent = maximum - minimum;
    if (!(extent > 0.0) || !std::isfinite(extent)) {
        return failure(0, "CPT numeric range is invalid");
    }

    std::vector<PointColorStop> stops;
    stops.reserve(intervals.size() * 2);
    const auto normalize = [minimum, extent](const double value) {
        return static_cast<float>((value - minimum) / extent);
    };
    for (const RawInterval &interval : intervals) {
        const PointColorStop lower{
            .position = normalize(interval.minimum),
            .color = interval.minimumColor,
        };
        if (stops.empty() || stops.back().color != lower.color) {
            stops.push_back(lower);
        }
        stops.push_back({
            .position = normalize(interval.maximum),
            .color = interval.maximumColor,
        });
    }

    return ParsedCptColorMap{
        .name = std::move(name),
        .description = std::move(description),
        .stops = std::move(stops),
    };
}

} // namespace pci
