#include "import/ogr/OgrRuntime.h"

#include <cpl_error.h>
#include <gdal.h>

#include <mutex>

namespace pci {
namespace {

thread_local std::string *activeError = nullptr;

void captureError(const CPLErr, const int, const char *message)
{
    if (activeError != nullptr && message != nullptr) {
        *activeError = message;
    }
}

} // namespace

void ensureOgrRegistered()
{
    static const bool registered = [] {
        GDALAllRegister();
        return true;
    }();
    static_cast<void>(registered);
}

OgrErrorScope::OgrErrorScope()
{
    activeError = &message_;
    CPLPushErrorHandler(captureError);
}

OgrErrorScope::~OgrErrorScope()
{
    CPLPopErrorHandler();
    activeError = nullptr;
}

std::string OgrErrorScope::message() const
{
    return message_;
}

} // namespace pci
