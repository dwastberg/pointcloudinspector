#include <pci/rendering/rhi/PointPicker.h>

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QSize>

#include <cstdint>
#include <cstring>

namespace {

void setStoredId(QByteArray &data,
                 const QSize size,
                 const int x,
                 const int y,
                 const std::uint32_t value)
{
    const qsizetype offset = static_cast<qsizetype>((y * size.width() + x) * 4);
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

TEST_CASE("pick readback chooses the nearest covered pixel", "[qt][renderer]")
{
    const QSize size(5, 5);
    QByteArray data(size.width() * size.height() * 4, '\0');
    setStoredId(data, size, 0, 0, 8);
    setStoredId(data, size, 2, 1, 4);

    const auto result = pci::PointPicker::decodeNearestId(data, size);
    REQUIRE(result.has_value());
    CHECK(*result == 3);
}

TEST_CASE("empty and malformed pick readbacks miss", "[qt][renderer]")
{
    const QSize size(5, 5);
    const QByteArray empty(size.width() * size.height() * 4, '\0');
    CHECK_FALSE(pci::PointPicker::decodeNearestId(empty, size));
    CHECK_FALSE(
        pci::PointPicker::decodeNearestId(QByteArray(3, '\0'), QSize(1, 1)));
    CHECK_FALSE(pci::PointPicker::decodeNearestId({}, QSize()));
}

TEST_CASE("pick readback honors a circular snap radius", "[qt][renderer]")
{
    const QSize size(5, 5);
    QByteArray data(size.width() * size.height() * 4, '\0');
    setStoredId(data, size, 4, 4, 3); // Outside a radius-two circle.
    setStoredId(data, size, 2, 0, 7);

    const auto result = pci::PointPicker::decodeNearestId(data, size, 2);
    REQUIRE(result.has_value());
    CHECK(*result == 6);
}

} // namespace
