#include "qualification/QualificationDiff.h"

#include "platform/QtPath.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <cmath>
#include <expected>
#include <iomanip>
#include <limits>
#include <map>
#include <ostream>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace pci::qualification {
namespace {

constexpr auto toleranceSchema = "pcinspector.qualification-tolerances.v1";
constexpr auto headlessSchema = "pcinspector.release-h.v1";
constexpr auto nativeSchema = "pcinspector.release-h.native.v1";

struct MeasurementField {
    std::string name;
    QString reportField;
};

struct SchemaDefinition {
    std::vector<MeasurementField> measurements;
    std::vector<QString> numberComparabilityFields;
    std::vector<QString> stringComparabilityFields;
    std::vector<QString> boolComparabilityFields;
    bool hasNativeLayerIdentity = false;
};

struct Tolerance {
    bool exact = false;
    double maximumRelativeIncrease = 0.0;
};

using ToleranceTable = std::map<std::string, Tolerance>;
using Error = QString;

std::expected<QJsonObject, Error> readObject(const std::filesystem::path &path,
                                             const QString &description)
{
    QFile file(pathToQString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            QStringLiteral("could not read %1 '%2': %3")
                .arg(description, file.fileName(), file.errorString()));
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return std::unexpected(
            QStringLiteral("invalid JSON in %1 '%2': %3")
                .arg(description, file.fileName(), parseError.errorString()));
    }
    if (!document.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 '%2' must contain a JSON object")
                .arg(description, file.fileName()));
    }
    return document.object();
}

std::expected<QString, Error> requiredString(const QJsonObject &object,
                                             const QString &field)
{
    const QJsonValue value = object.value(field);
    if (!value.isString() || value.toString().isEmpty()) {
        return std::unexpected(
            QStringLiteral("required field '%1' must be a non-empty string")
                .arg(field));
    }
    return value.toString();
}

std::expected<double, Error> requiredNumber(const QJsonObject &object,
                                            const QString &field)
{
    const QJsonValue value = object.value(field);
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        value.toDouble() < 0.0) {
        return std::unexpected(
            QStringLiteral(
                "required field '%1' must be a finite non-negative number")
                .arg(field));
    }
    return value.toDouble();
}

std::expected<bool, Error> requiredBool(const QJsonObject &object,
                                        const QString &field)
{
    const QJsonValue value = object.value(field);
    if (!value.isBool()) {
        return std::unexpected(
            QStringLiteral("required field '%1' must be a boolean").arg(field));
    }
    return value.toBool();
}

std::expected<ToleranceTable, Error>
readTolerances(const std::filesystem::path &path)
{
    const auto document = readObject(path, QStringLiteral("tolerance table"));
    if (!document) {
        return std::unexpected(document.error());
    }
    const auto schema = requiredString(*document, QStringLiteral("schema"));
    if (!schema) {
        return std::unexpected(schema.error());
    }
    if (*schema != QString::fromLatin1(toleranceSchema)) {
        return std::unexpected(
            QStringLiteral("unsupported tolerance schema '%1'").arg(*schema));
    }
    const QJsonValue measurementsValue =
        document->value(QStringLiteral("measurements"));
    if (!measurementsValue.isObject()) {
        return std::unexpected(
            QStringLiteral("tolerance field 'measurements' must be an object"));
    }

    ToleranceTable tolerances;
    const QJsonObject measurements = measurementsValue.toObject();
    for (auto iterator = measurements.begin(); iterator != measurements.end();
         ++iterator) {
        if (!iterator.value().isObject()) {
            return std::unexpected(
                QStringLiteral("tolerance for '%1' must be an object")
                    .arg(iterator.key()));
        }
        const QJsonObject specification = iterator.value().toObject();
        const bool hasExact = specification.contains(QStringLiteral("exact"));
        const bool hasRelative =
            specification.contains(QStringLiteral("maximum_relative_increase"));
        if (hasExact == hasRelative) {
            return std::unexpected(
                QStringLiteral("tolerance for '%1' must define exactly one of "
                               "'exact' or 'maximum_relative_increase'")
                    .arg(iterator.key()));
        }

        Tolerance tolerance;
        if (hasExact) {
            const auto exact =
                requiredBool(specification, QStringLiteral("exact"));
            if (!exact || !*exact) {
                return std::unexpected(
                    QStringLiteral("exact tolerance for '%1' must be true")
                        .arg(iterator.key()));
            }
            tolerance.exact = true;
        } else {
            const auto relative = requiredNumber(
                specification, QStringLiteral("maximum_relative_increase"));
            if (!relative) {
                return std::unexpected(
                    QStringLiteral("invalid tolerance for '%1': %2")
                        .arg(iterator.key(), relative.error()));
            }
            tolerance.maximumRelativeIncrease = *relative;
        }
        tolerances.emplace(iterator.key().toStdString(), tolerance);
    }
    return tolerances;
}

SchemaDefinition definitionFor(const QString &schema)
{
    if (schema == QString::fromLatin1(headlessSchema)) {
        return {
            .measurements =
                {
                    {"first_point_any", QStringLiteral("first_any_preview_ms")},
                    {"first_point_all", QStringLiteral("first_all_preview_ms")},
                    {"all_sources_display_ready", QStringLiteral("import_ms")},
                    {"peak_cpu_page_residency",
                     QStringLiteral("decoded_peak_bytes")},
                },
            .numberComparabilityFields =
                {
                    QStringLiteral("cpu_budget_bytes"),
                    QStringLiteral("source_count"),
                    QStringLiteral("source_file_bytes"),
                    QStringLiteral("persistent_index_bytes"),
                    QStringLiteral("persistent_sources"),
                    QStringLiteral("reused_persistent_sources"),
                },
            .stringComparabilityFields = {QStringLiteral("status")},
        };
    }
    if (schema == QString::fromLatin1(nativeSchema)) {
        return {
            .measurements =
                {
                    {"first_point_any", QStringLiteral("first_any_preview_ms")},
                    {"first_point_all", QStringLiteral("first_all_preview_ms")},
                    {"all_sources_display_ready",
                     QStringLiteral("display_ready_ms")},
                    {"sampled_frame_p95", QStringLiteral("frame_ms_p95")},
                    {"sampled_frame_max", QStringLiteral("frame_ms_max")},
                    {"peak_cpu_page_residency",
                     QStringLiteral("cpu_peak_bytes")},
                    {"peak_gpu_point_residency",
                     QStringLiteral("gpu_peak_bytes")},
                    {"settled_visible_draw_calls",
                     QStringLiteral("draw_calls")},
                    {"full_detail_hierarchy_pages",
                     QStringLiteral("full_detail_decoded_nodes")},
                    {"gpu_evictions", QStringLiteral("cache_evictions")},
                },
            .numberComparabilityFields =
                {
                    QStringLiteral("cpu_budget_bytes"),
                    QStringLiteral("gpu_budget_bytes"),
                    QStringLiteral("source_count"),
                    QStringLiteral("source_file_bytes"),
                    QStringLiteral("source_points"),
                    QStringLiteral("full_detail_total_nodes"),
                },
            .stringComparabilityFields =
                {
                    QStringLiteral("status"),
                    QStringLiteral("selected_backend"),
                },
            .boolComparabilityFields =
                {
                    QStringLiteral("gpu_validation"),
                    QStringLiteral("full_detail_active"),
                },
            .hasNativeLayerIdentity = true,
        };
    }
    return {};
}

QString normalizedStatus(const QString &schema, const QString &status)
{
    if (schema == QString::fromLatin1(nativeSchema) &&
        status.startsWith(QStringLiteral("Loaded "))) {
        return QStringLiteral("loaded");
    }
    return status;
}

using LayerIdentity = std::pair<double, double>;

std::expected<std::vector<LayerIdentity>, Error>
nativeLayerIdentity(const QJsonObject &report)
{
    const QJsonValue layersValue = report.value(QStringLiteral("layers"));
    if (!layersValue.isArray()) {
        return std::unexpected(
            QStringLiteral("required field 'layers' must be an array"));
    }
    std::vector<LayerIdentity> identity;
    const QJsonArray layers = layersValue.toArray();
    identity.reserve(static_cast<std::size_t>(layers.size()));
    for (qsizetype index = 0; index < layers.size(); ++index) {
        if (!layers[index].isObject()) {
            return std::unexpected(
                QStringLiteral("layers[%1] must be an object").arg(index));
        }
        const QJsonObject layer = layers[index].toObject();
        const auto bytes =
            requiredNumber(layer, QStringLiteral("source_file_bytes"));
        const auto points =
            requiredNumber(layer, QStringLiteral("source_points"));
        if (!bytes || !points) {
            return std::unexpected(
                QStringLiteral("layers[%1]: %2")
                    .arg(index)
                    .arg(!bytes ? bytes.error() : points.error()));
        }
        identity.emplace_back(*bytes, *points);
    }
    std::ranges::sort(identity);
    return identity;
}

template <typename Value>
std::expected<bool, Error> compareField(
    const QJsonObject &baseline,
    const QJsonObject &candidate,
    const QString &field,
    std::expected<Value, Error> (*reader)(const QJsonObject &, const QString &),
    std::ostream &errors)
{
    const auto baselineValue = reader(baseline, field);
    if (!baselineValue) {
        return std::unexpected(
            QStringLiteral("baseline: %1").arg(baselineValue.error()));
    }
    const auto candidateValue = reader(candidate, field);
    if (!candidateValue) {
        return std::unexpected(
            QStringLiteral("candidate: %1").arg(candidateValue.error()));
    }
    if (*baselineValue == *candidateValue) {
        return true;
    }
    errors << "WORKLOAD_MISMATCH " << field.toStdString()
           << " baseline=" << *baselineValue << " candidate=" << *candidateValue
           << '\n';
    return false;
}

template <>
std::expected<bool, Error> compareField<QString>(
    const QJsonObject &baseline,
    const QJsonObject &candidate,
    const QString &field,
    std::expected<QString, Error> (*reader)(const QJsonObject &,
                                            const QString &),
    std::ostream &errors)
{
    const auto baselineValue = reader(baseline, field);
    if (!baselineValue) {
        return std::unexpected(
            QStringLiteral("baseline: %1").arg(baselineValue.error()));
    }
    const auto candidateValue = reader(candidate, field);
    if (!candidateValue) {
        return std::unexpected(
            QStringLiteral("candidate: %1").arg(candidateValue.error()));
    }
    if (*baselineValue == *candidateValue) {
        return true;
    }
    errors << "WORKLOAD_MISMATCH " << field.toStdString() << " baseline='"
           << baselineValue->toStdString() << "' candidate='"
           << candidateValue->toStdString() << "'\n";
    return false;
}

std::expected<bool, Error>
validateComparability(const QJsonObject &baseline,
                      const QJsonObject &candidate,
                      const QString &schema,
                      const SchemaDefinition &definition,
                      std::ostream &errors)
{
    bool comparable = true;
    for (const QString &field : definition.numberComparabilityFields) {
        const auto result = compareField<double>(
            baseline, candidate, field, requiredNumber, errors);
        if (!result) {
            return std::unexpected(result.error());
        }
        comparable = comparable && *result;
    }
    for (const QString &field : definition.stringComparabilityFields) {
        if (field == QStringLiteral("status")) {
            const auto baselineStatus = requiredString(baseline, field);
            const auto candidateStatus = requiredString(candidate, field);
            if (!baselineStatus || !candidateStatus) {
                return std::unexpected(QStringLiteral("%1: %2").arg(
                    !baselineStatus ? QStringLiteral("baseline")
                                    : QStringLiteral("candidate"),
                    !baselineStatus ? baselineStatus.error()
                                    : candidateStatus.error()));
            }
            const QString baselineNormalized =
                normalizedStatus(schema, *baselineStatus);
            const QString candidateNormalized =
                normalizedStatus(schema, *candidateStatus);
            if (baselineNormalized != candidateNormalized) {
                errors << "WORKLOAD_MISMATCH status baseline='"
                       << baselineNormalized.toStdString() << "' candidate='"
                       << candidateNormalized.toStdString() << "'\n";
                comparable = false;
            }
            continue;
        }
        const auto result = compareField<QString>(
            baseline, candidate, field, requiredString, errors);
        if (!result) {
            return std::unexpected(result.error());
        }
        comparable = comparable && *result;
    }
    for (const QString &field : definition.boolComparabilityFields) {
        const auto result = compareField<bool>(
            baseline, candidate, field, requiredBool, errors);
        if (!result) {
            return std::unexpected(result.error());
        }
        comparable = comparable && *result;
    }
    if (definition.hasNativeLayerIdentity) {
        const auto baselineIdentity = nativeLayerIdentity(baseline);
        const auto candidateIdentity = nativeLayerIdentity(candidate);
        if (!baselineIdentity || !candidateIdentity) {
            return std::unexpected(QStringLiteral("%1: %2").arg(
                !baselineIdentity ? QStringLiteral("baseline")
                                  : QStringLiteral("candidate"),
                !baselineIdentity ? baselineIdentity.error()
                                  : candidateIdentity.error()));
        }
        if (*baselineIdentity != *candidateIdentity) {
            errors << "WORKLOAD_MISMATCH layer source metadata differs\n";
            comparable = false;
        }
    }
    return comparable;
}

void printSchemaError(std::ostream &errors, const QString &message)
{
    errors << "SCHEMA_ERROR " << message.toStdString() << '\n';
}

} // namespace

DiffExitCode compareReports(const std::filesystem::path &baselinePath,
                            const std::filesystem::path &candidatePath,
                            const std::filesystem::path &tolerancePath,
                            std::ostream &output,
                            std::ostream &errors)
{
    const auto baseline =
        readObject(baselinePath, QStringLiteral("baseline report"));
    const auto candidate =
        readObject(candidatePath, QStringLiteral("candidate report"));
    const auto tolerances = readTolerances(tolerancePath);
    if (!baseline || !candidate || !tolerances) {
        printSchemaError(errors,
                         !baseline    ? baseline.error()
                         : !candidate ? candidate.error()
                                      : tolerances.error());
        return DiffExitCode::SchemaError;
    }

    const auto baselineSchema =
        requiredString(*baseline, QStringLiteral("schema"));
    const auto candidateSchema =
        requiredString(*candidate, QStringLiteral("schema"));
    if (!baselineSchema || !candidateSchema) {
        printSchemaError(errors,
                         !baselineSchema ? baselineSchema.error()
                                         : candidateSchema.error());
        return DiffExitCode::SchemaError;
    }
    if (*baselineSchema != *candidateSchema) {
        errors << "WORKLOAD_MISMATCH schema baseline='"
               << baselineSchema->toStdString() << "' candidate='"
               << candidateSchema->toStdString() << "'\n";
        return DiffExitCode::WorkloadMismatch;
    }

    const SchemaDefinition definition = definitionFor(*baselineSchema);
    if (definition.measurements.empty()) {
        printSchemaError(errors,
                         QStringLiteral("unsupported report schema '%1'")
                             .arg(*baselineSchema));
        return DiffExitCode::SchemaError;
    }

    const auto comparable = validateComparability(
        *baseline, *candidate, *baselineSchema, definition, errors);
    if (!comparable) {
        printSchemaError(errors, comparable.error());
        return DiffExitCode::SchemaError;
    }
    if (!*comparable) {
        return DiffExitCode::WorkloadMismatch;
    }

    bool regressed = false;
    output << "schema=" << baselineSchema->toStdString() << '\n';
    for (const MeasurementField &measurement : definition.measurements) {
        const auto tolerance = tolerances->find(measurement.name);
        if (tolerance == tolerances->end()) {
            printSchemaError(
                errors,
                QStringLiteral("missing tolerance for measurement '%1'")
                    .arg(QString::fromStdString(measurement.name)));
            return DiffExitCode::SchemaError;
        }
        const auto baselineValue =
            requiredNumber(*baseline, measurement.reportField);
        const auto candidateValue =
            requiredNumber(*candidate, measurement.reportField);
        if (!baselineValue || !candidateValue) {
            printSchemaError(errors,
                             QStringLiteral("%1: %2").arg(
                                 !baselineValue ? QStringLiteral("baseline")
                                                : QStringLiteral("candidate"),
                                 !baselineValue ? baselineValue.error()
                                                : candidateValue.error()));
            return DiffExitCode::SchemaError;
        }

        bool passed = false;
        double relativeChange = 0.0;
        if (tolerance->second.exact) {
            passed = *candidateValue == *baselineValue;
        } else if (*baselineValue == 0.0) {
            passed = *candidateValue == 0.0;
            relativeChange =
                passed ? 0.0 : std::numeric_limits<double>::infinity();
        } else {
            relativeChange =
                (*candidateValue - *baselineValue) / *baselineValue;
            passed =
                relativeChange <= tolerance->second.maximumRelativeIncrease;
        }
        regressed = regressed || !passed;

        output << (passed ? "PASS " : "REGRESSION ") << measurement.name
               << " baseline=" << std::setprecision(12) << *baselineValue
               << " candidate=" << *candidateValue;
        if (tolerance->second.exact) {
            output << " tolerance=exact";
        } else {
            output << " change=";
            if (std::isfinite(relativeChange)) {
                output << std::showpos << std::setprecision(4)
                       << relativeChange * 100.0 << std::noshowpos << '%';
            } else {
                output << "+infinity";
            }
            output << " tolerance=+" << std::setprecision(4)
                   << tolerance->second.maximumRelativeIncrease * 100.0 << '%';
        }
        output << '\n';
    }
    return regressed ? DiffExitCode::Regression : DiffExitCode::Success;
}

} // namespace pci::qualification
