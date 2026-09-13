#pragma once

#include "novafix/plugin_api.h"

#include <string_view>

namespace novafix::compat::native_notifications {

struct Backend {
    bool (*canOpen)(){};
    void* (*open)(std::wstring_view text, bool reportFailure){};
    bool (*close)(void* product){};
};

// Configure support before add-on queries; the backend may be registered later
void Configure(bool supported);
void RegisterBackend(Backend backend);

uint32_t Queue(const char* utf8Text);
uint32_t QueueEx(const char* utf8Text,
                 const NovaFixPluginNativeMessageOptions* options);

// Called from a game UI/update point, never from Present or an add-on thread
// Only backend callbacks may touch native game objects
void Pump();
void NotifyClosed(void* product);

} // namespace novafix::compat::native_notifications
