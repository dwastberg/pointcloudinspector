#include "import/local/LocalPointPageFormat.h"

#include "foundation/CheckedArithmetic.h"
#include "pointcloud/GpuPointProperties.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace pci {
namespace {

constexpr std::array<std::byte, 8> manifestMagic{std::byte{'P'},
                                                 std::byte{'C'},
                                                 std::byte{'I'},
                                                 std::byte{'P'},
                                                 std::byte{'G'},
                                                 std::byte{'S'},
                                                 std::byte{'0'},
                                                 std::byte{'1'}};
constexpr std::uint32_t endianMarker = 0x01020304U;
constexpr std::uint64_t sourceSampleBytes = 64ULL * 1024;
constexpr std::uint8_t maximumSupportedLevel = 20;

template <typename Integer>
void appendInteger(std::vector<std::byte> &bytes, const Integer value)
{
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes.push_back(static_cast<std::byte>(bits & 0xffU));
        if constexpr (sizeof(Integer) > 1) {
            bits >>= 8U;
        }
    }
}

void appendDouble(std::vector<std::byte> &bytes, const double value)
{
    appendInteger(bytes, std::bit_cast<std::uint64_t>(value));
}

void appendString(std::vector<std::byte> &bytes, const std::string &value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("local page manifest string is too large");
    }
    appendInteger(bytes, static_cast<std::uint32_t>(value.size()));
    const auto *begin = reinterpret_cast<const std::byte *>(value.data());
    bytes.insert(bytes.end(), begin, begin + value.size());
}

void appendBounds(std::vector<std::byte> &bytes, const Bounds3d &bounds)
{
    for (const double value : bounds.minimum) {
        appendDouble(bytes, value);
    }
    for (const double value : bounds.maximum) {
        appendDouble(bytes, value);
    }
}

class ByteReader final {
public:
    explicit ByteReader(const std::span<const std::byte> bytes)
        : bytes_(bytes)
    {
    }

    template <typename Integer> [[nodiscard]] Integer integer()
    {
        if (remaining() < sizeof(Integer)) {
            throw std::runtime_error("truncated local page manifest");
        }
        using Unsigned = std::make_unsigned_t<Integer>;
        Unsigned result = 0;
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            result |= static_cast<Unsigned>(
                          std::to_integer<std::uint8_t>(bytes_[offset_++]))
                      << (index * 8U);
        }
        return static_cast<Integer>(result);
    }

    [[nodiscard]] double real()
    {
        return std::bit_cast<double>(integer<std::uint64_t>());
    }

    [[nodiscard]] std::string string()
    {
        const std::uint32_t size = integer<std::uint32_t>();
        if (remaining() < size) {
            throw std::runtime_error("invalid local page manifest string");
        }
        const auto *begin =
            reinterpret_cast<const char *>(bytes_.data() + offset_);
        offset_ += size;
        return std::string(begin, begin + size);
    }

    [[nodiscard]] Bounds3d bounds()
    {
        Bounds3d result;
        for (double &value : result.minimum) {
            value = real();
        }
        for (double &value : result.maximum) {
            value = real();
        }
        if (!result.valid()) {
            throw std::runtime_error("invalid bounds in local page manifest");
        }
        return result;
    }

    [[nodiscard]] std::span<const std::byte> take(const std::size_t size)
    {
        if (remaining() < size) {
            throw std::runtime_error("truncated local page manifest");
        }
        const auto result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return bytes_.size() - offset_;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

void hashBytes(QCryptographicHash &hash, const std::span<const std::byte> bytes)
{
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(bytes.data()),
                                static_cast<qsizetype>(bytes.size())));
}

void hashVector(QCryptographicHash &hash, const std::vector<std::byte> &bytes)
{
    hashBytes(hash, bytes);
}

std::filesystem::path canonicalPath(const std::filesystem::path &path)
{
    std::error_code error;
    std::filesystem::path result = std::filesystem::canonical(path, error);
    if (!error) {
        return result;
    }
    error.clear();
    result = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : result.lexically_normal();
}

std::string pathUtf8(const std::filesystem::path &path)
{
    const std::u8string encoded = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(encoded.data()),
                       encoded.size());
}

std::filesystem::path pathFromUtf8(const std::string &encoded)
{
    std::u8string value(encoded.size(), u8'\0');
    std::memcpy(value.data(), encoded.data(), encoded.size());
    return std::filesystem::path(value);
}

std::vector<std::uint64_t> sampleOffsets(const std::uint64_t fileBytes)
{
    if (fileBytes <= sourceSampleBytes) {
        return {0};
    }
    const std::uint64_t maximumOffset = fileBytes - sourceSampleBytes;
    std::vector<std::uint64_t> result{
        0,
        maximumOffset / 2,
        maximumOffset,
    };
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::byte> readFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("could not open local page manifest: " +
                                 path.string());
    }
    const std::streamoff end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) >
                       std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("local page manifest is too large");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        throw std::runtime_error("could not read local page manifest");
    }
    return bytes;
}

std::uint32_t metadataFlags(const PointCloudMetadata &metadata) noexcept
{
    return (metadata.hasColor ? 1U : 0U) | (metadata.hasIntensity ? 2U : 0U) |
           (metadata.hasClassification ? 4U : 0U) |
           (metadata.hasReturnNumber ? 8U : 0U) |
           (metadata.hasNumberOfReturns ? 16U : 0U);
}

void applyMetadataFlags(PointCloudMetadata &metadata, const std::uint32_t flags)
{
    if ((flags & ~31U) != 0) {
        throw std::runtime_error("unknown local page metadata flags");
    }
    metadata.hasColor = (flags & 1U) != 0;
    metadata.hasIntensity = (flags & 2U) != 0;
    metadata.hasClassification = (flags & 4U) != 0;
    metadata.hasReturnNumber = (flags & 8U) != 0;
    metadata.hasNumberOfReturns = (flags & 16U) != 0;
}

bool recordOrder(const LocalPointPageRecord &left,
                 const LocalPointPageRecord &right) noexcept
{
    return left.id < right.id;
}

} // namespace

LocalPointSourceFingerprint
fingerprintLocalPointSource(const PointCloudMetadata &metadata,
                            const std::uint64_t sourceFileBytes,
                            const std::uint64_t sourceModificationTime,
                            const LocalPointPageStoreOptions &options)
{
    if (options.pointsPerLeaf == 0 || options.rootPreviewPoints == 0) {
        throw std::invalid_argument(
            "local page layout point counts must be positive");
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::vector<std::byte> identity;
    appendInteger(identity, localPointPageFormatVersion);
    appendInteger(identity, localPointPageSchemaVersion);
    appendInteger(identity, localPointPageBuildRevision);
    appendInteger(identity, options.pointsPerLeaf);
    appendInteger(identity, options.rootPreviewPoints);
    appendString(identity, pathUtf8(canonicalPath(metadata.sourcePath)));
    appendInteger(identity, sourceFileBytes);
    appendInteger(identity, sourceModificationTime);
    appendString(identity, metadata.sourceDriver);
    appendInteger(identity, metadata.sourcePointCount);
    appendBounds(identity, metadata.sourceBounds);
    appendInteger(identity, metadataFlags(metadata));
    appendString(identity, metadata.spatialReferenceWkt);
    appendInteger(identity,
                  static_cast<std::uint64_t>(metadata.dimensions.size()));
    for (const std::string &dimension : metadata.dimensions) {
        appendString(identity, dimension);
    }
    hashVector(hash, identity);

    std::ifstream source(metadata.sourcePath, std::ios::binary);
    if (!source) {
        throw std::runtime_error("could not fingerprint source: " +
                                 metadata.sourcePath.string());
    }
    for (const std::uint64_t offset : sampleOffsets(sourceFileBytes)) {
        source.clear();
        source.seekg(static_cast<std::streamoff>(offset));
        const std::uint64_t wanted =
            std::min(sourceSampleBytes, sourceFileBytes - offset);
        std::vector<std::byte> sample(static_cast<std::size_t>(wanted));
        if (!sample.empty()) {
            source.read(reinterpret_cast<char *>(sample.data()),
                        static_cast<std::streamsize>(sample.size()));
        }
        if (!source && !source.eof()) {
            throw std::runtime_error(
                "could not read source fingerprint sample");
        }
        identity.clear();
        appendInteger(identity, offset);
        appendInteger(identity, static_cast<std::uint64_t>(sample.size()));
        hashVector(hash, identity);
        hashVector(hash, sample);
    }

    const QByteArray digest = hash.result();
    if (digest.size() != 32) {
        throw std::runtime_error("SHA-256 produced an invalid digest");
    }
    LocalPointSourceFingerprint result{};
    std::memcpy(result.data(), digest.constData(), result.size());
    return result;
}

std::string
localPointFingerprintHex(const LocalPointSourceFingerprint &fingerprint)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(fingerprint.size() * 2);
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        result[index * 2] = digits[fingerprint[index] >> 4U];
        result[index * 2 + 1] = digits[fingerprint[index] & 0x0fU];
    }
    return result;
}

std::uint64_t localPointCurrentProcessId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

bool localPointProcessAlive(const std::uint64_t processId) noexcept
{
    if (processId == 0) {
        return false;
    }
#if defined(_WIN32)
    const HANDLE process =
        OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_TIMEOUT;
#else
    if (processId >
        static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return false;
    }
    errno = 0;
    return kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
#endif
}

std::uint32_t localPointCrc32(const std::span<const std::byte> bytes,
                              const std::uint32_t previous) noexcept
{
    std::uint32_t crc = ~previous;
    for (const std::byte byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::array<std::byte, localPointDiskBytes>
encodeLocalPoint(const PointSample &point) noexcept
{
    std::vector<std::byte> bytes;
    bytes.reserve(localPointDiskBytes);
    appendDouble(bytes, point.position.x);
    appendDouble(bytes, point.position.y);
    appendDouble(bytes, point.position.z);
    appendInteger(bytes, point.rgba);
    appendInteger(bytes, point.packedAttributes);
    appendInteger(bytes, point.packedProperties);
    std::array<std::byte, localPointDiskBytes> result{};
    std::ranges::copy(bytes, result.begin());
    return result;
}

PointSample
decodeLocalPoint(const std::span<const std::byte, localPointDiskBytes> bytes)
{
    ByteReader reader(bytes);
    PointSample result;
    result.position = {reader.real(), reader.real(), reader.real()};
    result.rgba = reader.integer<std::uint32_t>();
    result.packedAttributes = reader.integer<std::uint16_t>();
    result.packedProperties = reader.integer<std::uint32_t>();
    result.attributes = {
        .intensity = gpuPointIntensity(result.packedProperties),
        .classification =
            static_cast<std::uint8_t>(result.packedAttributes & 0xffU),
        .returnNumber = gpuPointReturnNumber(result.packedProperties),
        .numberOfReturns = gpuPointNumberOfReturns(result.packedProperties),
    };
    if (!isFinite(result.position)) {
        throw std::runtime_error("non-finite point in local page payload");
    }
    return result;
}

void writeLocalPointManifest(const std::filesystem::path &path,
                             const LocalPointPageManifest &manifest)
{
    if (!manifest.metadata.sourceBounds.valid() ||
        manifest.maximumLevel > maximumSupportedLevel ||
        manifest.pointsPerLeaf == 0 || manifest.rootPreviewPoints == 0) {
        throw std::invalid_argument("invalid local page manifest metadata");
    }
    std::vector<LocalPointPageRecord> pages = manifest.pages;
    std::ranges::sort(pages, recordOrder);
    if (pages.empty() || pages.front().id != rootPointCloudNode) {
        throw std::invalid_argument("local page manifest requires a root page");
    }

    std::vector<std::byte> bytes;
    bytes.insert(bytes.end(), manifestMagic.begin(), manifestMagic.end());
    appendInteger(bytes, localPointPageFormatVersion);
    appendInteger(bytes, localPointPageSchemaVersion);
    appendInteger(bytes, endianMarker);
    for (const std::uint8_t value : manifest.fingerprint) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    appendInteger(bytes, manifest.sourceFileBytes);
    appendInteger(bytes, manifest.sourceModificationTime);
    appendInteger(bytes, manifest.metadata.sourcePointCount);
    appendBounds(bytes, manifest.metadata.sourceBounds);
    appendInteger(bytes, manifest.maximumLevel);
    appendInteger(bytes, manifest.pointsPerLeaf);
    appendInteger(bytes, manifest.rootPreviewPoints);
    appendInteger(bytes, metadataFlags(manifest.metadata));
    appendInteger(bytes, manifest.scalarRanges.intensityMinimum);
    appendInteger(bytes, manifest.scalarRanges.intensityMaximum);
    appendInteger(bytes, manifest.scalarRanges.classificationMinimum);
    appendInteger(bytes, manifest.scalarRanges.classificationMaximum);
    appendInteger(bytes, manifest.scalarRanges.returnNumberMinimum);
    appendInteger(bytes, manifest.scalarRanges.returnNumberMaximum);
    appendString(bytes, pathUtf8(manifest.canonicalSourcePath));
    appendString(bytes, manifest.metadata.sourceDriver);
    appendString(bytes, manifest.metadata.spatialReferenceWkt);
    appendInteger(
        bytes, static_cast<std::uint64_t>(manifest.metadata.dimensions.size()));
    for (const std::string &dimension : manifest.metadata.dimensions) {
        appendString(bytes, dimension);
    }
    appendInteger(bytes, manifest.payloadFileBytes);
    appendInteger(bytes, static_cast<std::uint64_t>(pages.size()));
    for (const LocalPointPageRecord &page : pages) {
        appendInteger(bytes, page.id.level);
        appendInteger(bytes, page.id.x);
        appendInteger(bytes, page.id.y);
        appendInteger(bytes, page.id.z);
        appendBounds(bytes, page.tightBounds);
        appendInteger(bytes, page.sourcePointCount);
        appendInteger(bytes, page.pointCount);
        appendInteger(bytes, page.payloadOffset);
        appendInteger(bytes, page.payloadBytes);
        appendInteger(bytes, page.payloadChecksum);
    }
    appendInteger(bytes, localPointCrc32(bytes));

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create local page manifest: " +
                                 path.string());
    }
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("could not write local page manifest");
    }
}

LocalPointPageManifest
readLocalPointManifest(const std::filesystem::path &path,
                       const LocalPointSourceFingerprint &expectedFingerprint)
{
    const std::vector<std::byte> bytes = readFile(path);
    if (bytes.size() < manifestMagic.size() + sizeof(std::uint32_t)) {
        throw std::runtime_error("truncated local page manifest");
    }
    const std::uint32_t storedChecksum = [&] {
        ByteReader tail(std::span(bytes).last(sizeof(std::uint32_t)));
        return tail.integer<std::uint32_t>();
    }();
    if (localPointCrc32(std::span(bytes).first(
            bytes.size() - sizeof(std::uint32_t))) != storedChecksum) {
        throw std::runtime_error("local page manifest checksum mismatch");
    }

    ByteReader reader(
        std::span(bytes).first(bytes.size() - sizeof(std::uint32_t)));
    if (!std::ranges::equal(reader.take(manifestMagic.size()), manifestMagic)) {
        throw std::runtime_error("local page manifest magic mismatch");
    }
    if (reader.integer<std::uint32_t>() != localPointPageFormatVersion ||
        reader.integer<std::uint32_t>() != localPointPageSchemaVersion) {
        throw std::runtime_error("unsupported local page manifest version");
    }
    if (reader.integer<std::uint32_t>() != endianMarker) {
        throw std::runtime_error("local page manifest endian mismatch");
    }

    LocalPointPageManifest manifest;
    const auto fingerprint = reader.take(manifest.fingerprint.size());
    std::ranges::transform(
        fingerprint, manifest.fingerprint.begin(), [](const std::byte value) {
            return std::to_integer<std::uint8_t>(value);
        });
    if (manifest.fingerprint != expectedFingerprint) {
        throw std::runtime_error("local page source fingerprint mismatch");
    }
    manifest.sourceFileBytes = reader.integer<std::uint64_t>();
    manifest.sourceModificationTime = reader.integer<std::uint64_t>();
    manifest.metadata.sourcePointCount = reader.integer<std::uint64_t>();
    manifest.metadata.sourceBounds = reader.bounds();
    manifest.maximumLevel = reader.integer<std::uint8_t>();
    manifest.pointsPerLeaf = reader.integer<std::uint32_t>();
    manifest.rootPreviewPoints = reader.integer<std::uint32_t>();
    if (manifest.maximumLevel > maximumSupportedLevel ||
        manifest.pointsPerLeaf == 0 || manifest.rootPreviewPoints == 0) {
        throw std::runtime_error("invalid local page hierarchy metadata");
    }
    applyMetadataFlags(manifest.metadata, reader.integer<std::uint32_t>());
    manifest.scalarRanges.intensityMinimum = reader.integer<std::uint16_t>();
    manifest.scalarRanges.intensityMaximum = reader.integer<std::uint16_t>();
    manifest.scalarRanges.classificationMinimum =
        reader.integer<std::uint8_t>();
    manifest.scalarRanges.classificationMaximum =
        reader.integer<std::uint8_t>();
    manifest.scalarRanges.returnNumberMinimum = reader.integer<std::uint8_t>();
    manifest.scalarRanges.returnNumberMaximum = reader.integer<std::uint8_t>();
    if (manifest.scalarRanges.intensityMinimum >
            manifest.scalarRanges.intensityMaximum ||
        manifest.scalarRanges.classificationMinimum >
            manifest.scalarRanges.classificationMaximum ||
        manifest.scalarRanges.returnNumberMinimum >
            manifest.scalarRanges.returnNumberMaximum) {
        throw std::runtime_error("invalid local page scalar ranges");
    }
    manifest.canonicalSourcePath = pathFromUtf8(reader.string());
    manifest.metadata.sourcePath = manifest.canonicalSourcePath;
    manifest.metadata.sourceDriver = reader.string();
    manifest.metadata.spatialReferenceWkt = reader.string();
    const std::uint64_t dimensionCount = reader.integer<std::uint64_t>();
    if (dimensionCount > reader.remaining() / sizeof(std::uint32_t)) {
        throw std::runtime_error("invalid local page dimension count");
    }
    manifest.metadata.dimensions.reserve(
        static_cast<std::size_t>(dimensionCount));
    for (std::uint64_t index = 0; index < dimensionCount; ++index) {
        manifest.metadata.dimensions.push_back(reader.string());
    }
    manifest.payloadFileBytes = reader.integer<std::uint64_t>();
    const std::uint64_t pageCount = reader.integer<std::uint64_t>();
    constexpr std::size_t minimumPageRecordBytes = 97;
    if (pageCount == 0 ||
        pageCount > reader.remaining() / minimumPageRecordBytes) {
        throw std::runtime_error("invalid local page table size");
    }
    manifest.pages.reserve(static_cast<std::size_t>(pageCount));
    std::unordered_set<PointCloudNodeId, PointCloudNodeIdHash> ids;
    for (std::uint64_t index = 0; index < pageCount; ++index) {
        LocalPointPageRecord page;
        page.id.level = reader.integer<std::uint8_t>();
        page.id.x = reader.integer<std::uint32_t>();
        page.id.y = reader.integer<std::uint32_t>();
        page.id.z = reader.integer<std::uint32_t>();
        page.tightBounds = reader.bounds();
        page.sourcePointCount = reader.integer<std::uint64_t>();
        page.pointCount = reader.integer<std::uint64_t>();
        page.payloadOffset = reader.integer<std::uint64_t>();
        page.payloadBytes = reader.integer<std::uint64_t>();
        page.payloadChecksum = reader.integer<std::uint32_t>();
        const std::uint64_t cells = std::uint64_t{1} << page.id.level;
        const auto expectedPayloadBytes = checkedMultiply<std::uint64_t>(
            page.pointCount, localPointDiskBytes);
        if (page.id.level > manifest.maximumLevel || page.id.x >= cells ||
            page.id.y >= cells || page.id.z >= cells || page.pointCount == 0 ||
            !expectedPayloadBytes ||
            page.payloadBytes != *expectedPayloadBytes ||
            page.payloadOffset > manifest.payloadFileBytes ||
            page.payloadBytes >
                manifest.payloadFileBytes - page.payloadOffset ||
            !ids.insert(page.id).second) {
            throw std::runtime_error("invalid local page table record");
        }
        manifest.pages.push_back(page);
    }
    if (reader.remaining() != 0) {
        throw std::runtime_error("unexpected trailing local page metadata");
    }
    std::ranges::sort(manifest.pages, [](const auto &left, const auto &right) {
        return left.payloadOffset < right.payloadOffset;
    });
    std::uint64_t previousEnd = 0;
    for (const LocalPointPageRecord &page : manifest.pages) {
        if (page.payloadOffset < previousEnd) {
            throw std::runtime_error("overlapping local page payloads");
        }
        previousEnd = page.payloadOffset + page.payloadBytes;
    }
    if (std::ranges::none_of(manifest.pages, [](const auto &page) {
            return page.id == rootPointCloudNode;
        })) {
        throw std::runtime_error("local page root is missing");
    }
    std::ranges::sort(manifest.pages, recordOrder);
    return manifest;
}

} // namespace pci
