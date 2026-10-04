#include <pci/rendering/rhi/RhiResource.h>

#include <catch2/catch_test_macros.hpp>

namespace {

struct TrackedResource {
    explicit TrackedResource(bool &destroyed)
        : destroyed(destroyed)
    {
    }

    ~TrackedResource()
    {
        destroyed = true;
    }

    bool &destroyed;
};

struct ReleasedResource {
    explicit ReleasedResource(bool &released)
        : released(released)
    {
    }

    void release() noexcept
    {
        released = true;
    }

    bool &released;
};

TEST_CASE("RHI resource owners delete normal resources",
          "[unit][renderer][resource]")
{
    bool destroyed = false;
    {
        pci::RhiResourcePtr<TrackedResource> resource(
            new TrackedResource(destroyed));
        CHECK_FALSE(destroyed);
    }
    CHECK(destroyed);
}

TEST_CASE("RHI release deleter uses the resource release contract",
          "[unit][renderer][resource]")
{
    bool released = false;
    {
        std::unique_ptr<ReleasedResource,
                        pci::RhiReleaseDeleter<ReleasedResource>>
            resource(new ReleasedResource(released));
        CHECK_FALSE(released);
    }
    CHECK(released);
}

} // namespace
