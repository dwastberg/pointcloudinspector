#include "app/LoadingProgressModel.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace std::chrono_literals;

namespace {

TEST_CASE("loading progress reserves visible room for display preparation",
          "[unit][app]")
{
    pci::LoadingProgressModel model;
    model.reset();

    const auto reading =
        model.updateImport(pci::PointCloudImportStage::Reading, 5, 10);
    CHECK(reading.phase == pci::LoadingProgressPhase::Reading);
    CHECK(reading.percentage == 35);
    CHECK(reading.completed == 5);
    CHECK(reading.total == 10);
    CHECK_FALSE(reading.estimated);

    const auto optimizing =
        model.updateImport(pci::PointCloudImportStage::Optimizing, 10, 10);
    CHECK(optimizing.phase == pci::LoadingProgressPhase::Optimizing);
    CHECK(optimizing.percentage == 70);
    CHECK(optimizing.estimated);

    const auto halfway = model.advance(2100ms);
    CHECK(halfway.percentage == 72);
    CHECK(halfway.estimated);

    const auto capped = model.advance(20s);
    CHECK(capped.percentage == 74);
}

TEST_CASE("renderer progress cannot overtake active source reading",
          "[unit][app]")
{
    pci::LoadingProgressModel model;
    const auto reading =
        model.updateImport(pci::PointCloudImportStage::Reading, 5, 10);
    CHECK(reading.percentage == 35);

    const auto prematurePreview = model.updateRender({
        .stage = pci::RenderLoadStage::FirstFrameReady,
        .completed = 1,
        .total = 1,
    });
    CHECK(prematurePreview.phase == pci::LoadingProgressPhase::Reading);
    CHECK(prematurePreview.percentage == 35);

    const auto prematurePreparing = model.updateRender({
        .stage = pci::RenderLoadStage::Preparing,
        .completed = 10,
        .total = 10,
    });
    CHECK(prematurePreparing.phase == pci::LoadingProgressPhase::Reading);
    CHECK(prematurePreparing.percentage == 35);
    CHECK_FALSE(prematurePreparing.estimated);

    const auto resumed =
        model.updateImport(pci::PointCloudImportStage::Reading, 10, 10);
    CHECK(resumed.phase == pci::LoadingProgressPhase::PreparingRenderer);
    CHECK(resumed.percentage == 75);
    const auto preparing = model.updateRender({
        .stage = pci::RenderLoadStage::Preparing,
        .completed = 10,
        .total = 10,
    });
    CHECK(preparing.phase == pci::LoadingProgressPhase::PreparingRenderer);
    CHECK(preparing.percentage == 75);
    CHECK(preparing.estimated);
    CHECK(model.advance(1600ms).percentage == 79);

    const auto upload = model.updateRender({
        .stage = pci::RenderLoadStage::Uploading,
        .completed = 3,
        .total = 10,
    });
    CHECK(upload.phase == pci::LoadingProgressPhase::Uploading);
    CHECK(upload.percentage == 82);
    CHECK_FALSE(upload.estimated);

    const auto preview = model.updateRender({
        .stage = pci::RenderLoadStage::FirstFrameReady,
        .completed = 10,
        .total = 10,
    });
    CHECK(preview.phase == pci::LoadingProgressPhase::FirstFrameReady);
    CHECK(preview.percentage == 82);
    CHECK_FALSE(preview.estimated);

    const auto complete = model.updateRender({
        .stage = pci::RenderLoadStage::DisplayReady,
        .completed = 10,
        .total = 10,
    });
    CHECK(complete.phase == pci::LoadingProgressPhase::DisplayReady);
    CHECK(complete.percentage == 100);

    const auto stale = model.updateRender({
        .stage = pci::RenderLoadStage::Uploading,
        .completed = 0,
        .total = 10,
    });
    CHECK(stale.percentage == 100);
}

TEST_CASE("loading progress is monotonic from zero through display readiness",
          "[unit][app][loading][progress]")
{
    pci::LoadingProgressModel model;
    std::vector<int> percentages{model.state().percentage};
    percentages.push_back(
        model.updateImport(pci::PointCloudImportStage::Reading, 5, 10)
            .percentage);
    percentages.push_back(
        model.updateImport(pci::PointCloudImportStage::Reading, 10, 10)
            .percentage);
    percentages.push_back(
        model.updateImport(pci::PointCloudImportStage::Optimizing, 0, 10)
            .percentage);
    percentages.push_back(model.advance(20s).percentage);
    percentages.push_back(model
                              .updateRender({
                                  .stage = pci::RenderLoadStage::Uploading,
                                  .completed = 2,
                                  .total = 10,
                              })
                              .percentage);
    percentages.push_back(model
                              .updateRender({
                                  .stage = pci::RenderLoadStage::Uploading,
                                  .completed = 8,
                                  .total = 10,
                              })
                              .percentage);
    percentages.push_back(model
                              .updateRender({
                                  .stage = pci::RenderLoadStage::DisplayReady,
                                  .completed = 10,
                                  .total = 10,
                              })
                              .percentage);

    CHECK(percentages.front() == 0);
    CHECK(percentages.back() == 100);
    CHECK(std::ranges::is_sorted(percentages));
}

TEST_CASE("cached paged loading is driven by decode and upload work",
          "[unit][app][loading][progress][paging]")
{
    pci::LoadingProgressModel model;
    model.setPagedSource(true);
    CHECK(model.completeImport().percentage == 5);

    const auto preparing = model.updateRender({
        .stage = pci::RenderLoadStage::Preparing,
        .total = 10,
    });
    CHECK(preparing.percentage == 5);

    const auto coarseUpload = model.updateRender({
        .stage = pci::RenderLoadStage::Uploading,
        .completed = 1,
        .total = 10,
    });
    CHECK(coarseUpload.percentage == 14);
    const auto firstFrame = model.updateRender({
        .stage = pci::RenderLoadStage::FirstFrameReady,
        .completed = 1,
        .total = 10,
    });
    CHECK(firstFrame.percentage == 14);
    CHECK(model
              .updateRender({
                  .stage = pci::RenderLoadStage::DisplayReady,
                  .completed = 10,
                  .total = 10,
              })
              .percentage == 100);
}

TEST_CASE("first-time paged index construction receives measured progress",
          "[unit][app][loading][progress][paging]")
{
    pci::LoadingProgressModel model;
    model.setPagedSource(true);
    CHECK(model.updateImport(pci::PointCloudImportStage::Reading, 5, 10)
              .percentage == 22);
    CHECK(model.updateImport(pci::PointCloudImportStage::Reading, 10, 10)
              .percentage == 45);
    CHECK(model.updateImport(pci::PointCloudImportStage::Optimizing, 0, 10)
              .percentage == 45);
    CHECK(model.updateImport(pci::PointCloudImportStage::Optimizing, 10, 10)
              .percentage == 55);
    CHECK(model.completeImport().percentage == 55);
}

} // namespace
