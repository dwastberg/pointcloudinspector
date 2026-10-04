#pragma once

#include <expected>
#include <string>

namespace pci {

enum class JobErrorCode {
    Cancelled,
    Validation,
    ResourceAdmission,
    Io,
    UnsupportedSource,
    Internal,
};

struct JobError {
    JobErrorCode code = JobErrorCode::Internal;
    std::string message;

    bool operator==(const JobError &) const = default;
};

template <typename T> using JobResult = std::expected<T, JobError>;

} // namespace pci
