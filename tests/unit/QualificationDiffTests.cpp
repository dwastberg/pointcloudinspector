#include "qualification/QualificationDiff.h"
#include <pci/adapters/platform/QtPath.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <filesystem>
#include <sstream>

namespace {

const std::filesystem::path fixtures{PCI_QUALIFICATION_FIXTURE_DIR};

QJsonObject nativeReport()
{
    QFile file(pci::pathToQString(fixtures / "native.json"));
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(file.readAll());
    REQUIRE(document.isObject());
    return document.object();
}

void checkCandidate(const QJsonObject &candidate,
                    pci::qualification::DiffExitCode expected,
                    const std::string &diagnostic)
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath("candidate.json");
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    const auto bytes = QJsonDocument(candidate).toJson();
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();

    std::ostringstream output;
    std::ostringstream errors;
    const auto result =
        pci::qualification::compareReports(fixtures / "native.json",
                                           pci::qStringToPath(path),
                                           fixtures / "tolerances.json",
                                           output,
                                           errors);
    INFO(output.str());
    INFO(errors.str());
    CHECK(result == expected);
    CHECK((output.str() + errors.str()).find(diagnostic) != std::string::npos);
}

} // namespace

TEST_CASE("Qualification accepts identical supported reports",
          "[qualification]")
{
    const auto report =
        GENERATE("headless.json", "headless-reopen.json", "native.json");
    CAPTURE(report);
    std::ostringstream output;
    std::ostringstream errors;
    const auto result =
        pci::qualification::compareReports(fixtures / report,
                                           fixtures / report,
                                           fixtures / "tolerances.json",
                                           output,
                                           errors);
    INFO(errors.str());
    CHECK(result == pci::qualification::DiffExitCode::Success);
    CHECK(output.str().find("PASS first_point_any") != std::string::npos);
    CHECK(errors.str().empty());
}

TEST_CASE("Qualification ignores timestamps and source paths",
          "[qualification]")
{
    auto candidate = nativeReport();
    candidate["timestamp_utc"] = "2099-01-01T00:00:00.000Z";
    auto layers = candidate["layers"].toArray();
    REQUIRE_FALSE(layers.isEmpty());
    for (qsizetype index = 0; index < layers.size(); ++index) {
        auto layer = layers[index].toObject();
        layer["path"] = QString("synthetic/ignored-source-%1.laz").arg(index);
        layers[index] = layer;
    }
    candidate["layers"] = layers;
    checkCandidate(candidate,
                   pci::qualification::DiffExitCode::Success,
                   "PASS first_point_any");
}

TEST_CASE("Qualification rejects performance regressions", "[qualification]")
{
    auto candidate = nativeReport();
    candidate["frame_ms_p95"] = candidate["frame_ms_p95"].toDouble() * 2.0;
    checkCandidate(candidate,
                   pci::qualification::DiffExitCode::Regression,
                   "REGRESSION sampled_frame_p95");
}

TEST_CASE("Qualification rejects incomparable workloads", "[qualification]")
{
    auto candidate = nativeReport();
    candidate["gpu_budget_bytes"] =
        candidate["gpu_budget_bytes"].toDouble() + 1.0;
    checkCandidate(candidate,
                   pci::qualification::DiffExitCode::WorkloadMismatch,
                   "WORKLOAD_MISMATCH gpu_budget_bytes");
}

TEST_CASE("Qualification rejects reports missing required measurements",
          "[qualification]")
{
    auto candidate = nativeReport();
    candidate.remove("frame_ms_p95");
    checkCandidate(candidate,
                   pci::qualification::DiffExitCode::SchemaError,
                   "SCHEMA_ERROR candidate: required field 'frame_ms_p95'");
}
