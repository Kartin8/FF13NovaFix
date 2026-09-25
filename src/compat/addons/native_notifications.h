#pragma once

#include "novafix/plugin_api.h"

#include <string_view>

namespace novafix::compat::native_notifications {

struct OpenRequest {
    std::uint32_t mode{};
    std::uint32_t durationMs{};
    int horizontalAlignment{1};
    int verticalAlignment{1};
    bool exclusiveInput{};
};

struct Backend {
    bool (*canOpen)(){};
    void* (*open)(std::wstring_view text, const OpenRequest& request,
                  bool reportFailure){};
    bool (*close)(void* product){};
    bool (*focusChoice)(void* product){};
};

// Configure support before add-on queries; the backend may be registered later
void Configure(bool supported, bool inlineGlyphsSupported);
void RegisterBackend(Backend backend);

uint32_t Queue(const char* utf8Text);
uint32_t QueueEx(const char* utf8Text,
                 const NovaFixPluginNativeMessageOptions* options);
uint32_t Enqueue(
    const char* utf8Text,
    const NovaFixPluginNativeNotificationOptions* options);

const NovaFixPluginNativeNotificationService* Service();

// Called from a game UI/update point, never from Present or an add-on thread
// Only backend callbacks may touch native game objects
void Pump();
void NotifyClosed(void* product);
bool ShouldSuppressClose(void* product);
bool BlocksGameplayMovement();
bool BlocksGameplayInput();

} // namespace novafix::compat::native_notifications
