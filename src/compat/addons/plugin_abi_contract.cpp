#include "novafix/plugin.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(sizeof(void*) == 4u);

static_assert(offsetof(NovaFixPluginNativeMenuItem, userData) == 28u);
static_assert(sizeof(NovaFixPluginNativeMenuItem) == 32u);
static_assert(offsetof(NovaFixPluginNativeMenu, items) == 20u);
static_assert(sizeof(NovaFixPluginNativeMenu) == 24u);

static_assert(offsetof(NovaFixPluginControl, extensions) == 56u);
static_assert(offsetof(NovaFixPluginDescriptor, author) == 40u);
static_assert(offsetof(NovaFixPluginHost, hostContext) == 28u);
static_assert(sizeof(NovaFixPluginControl) == 60u);
static_assert(sizeof(NovaFixPluginDescriptor) == 76u);
static_assert(sizeof(NovaFixPluginHost) == 36u);

static_assert(offsetof(NovaFixPluginControl, extensions) ==
              sizeof(std::uint32_t) * 6u + sizeof(float) * 3u +
                  sizeof(void*) * 5u);
static_assert(offsetof(NovaFixPluginDescriptor, author) ==
              sizeof(std::uint32_t) * 6u + sizeof(void*) * 4u);
static_assert(offsetof(NovaFixPluginHost, hostContext) ==
              sizeof(std::uint32_t) * 3u + sizeof(void*) * 4u);

static_assert(sizeof(NovaFixPluginExtension) == 16u);
static_assert(sizeof(NovaFixPluginControlStateExtension) == 24u);
static_assert(sizeof(NovaFixPluginStatusTextExtension) == 28u);
static_assert(sizeof(NovaFixPluginColorExtension) == 32u);
static_assert(sizeof(NovaFixPluginPresentationExtension) == 20u);
static_assert(sizeof(NovaFixPluginValueDisplayExtension) == 28u);
static_assert(sizeof(NovaFixPluginServiceHeader) == 8u);
static_assert(sizeof(NovaFixPluginLogService) == 16u);
static_assert(offsetof(NovaFixPluginNativeNotificationOptions, position) ==
              20u);
static_assert(sizeof(NovaFixPluginNativeNotificationOptions) == 24u);
static_assert(sizeof(NovaFixPluginNativeNotificationService) == 20u);

static_assert(std::is_trivially_copyable_v<NovaFixPluginControl>);
static_assert(std::is_trivially_copyable_v<NovaFixPluginDescriptor>);
static_assert(std::is_trivially_copyable_v<NovaFixPluginHost>);
static_assert(std::is_trivially_copyable_v<
              NovaFixPluginNativeNotificationOptions>);

using OneControl = NovaFixPluginControl[1];
static_assert(std::is_nothrow_constructible_v<
              novafix::Plugin, const char*, const char*, const OneControl&,
              novafix::PluginMetadata>);

using OneCppControl = novafix::Control[1];
static_assert(std::is_nothrow_constructible_v<
              novafix::Plugin, const char*, const char*,
              const OneCppControl&, novafix::PluginMetadata>);
