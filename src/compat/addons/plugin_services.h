#pragma once

#include "novafix/plugin_api.h"

namespace novafix::compat::plugin_services {

const void* NOVAFIX_PLUGIN_CALL GetService(
    void* hostContext, const char* serviceId, uint32_t minimumVersion);

} // namespace novafix::compat::plugin_services
