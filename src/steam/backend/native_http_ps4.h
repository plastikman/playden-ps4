#pragma once

#include "steam/backend/http_client.h"

namespace playden::steam::internal {

bool InitializeNativeHttpPs4();
HttpResponse PerformNativeHttpPs4(
    const HttpRequest& request, const std::atomic<bool>& cancel,
    const std::function<void(const std::string&)>& trace);

}  // namespace playden::steam::internal
