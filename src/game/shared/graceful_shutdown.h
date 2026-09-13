#pragma once

namespace novafix::game::shutdown {

enum class RequestResult {
    Requested,
    Unsupported,
    NotReady,
    ContractMismatch,
};

void Pump();
RequestResult RequestApplicationShutdown();
const char* RequestResultName(RequestResult result);

} // namespace novafix::game::shutdown
