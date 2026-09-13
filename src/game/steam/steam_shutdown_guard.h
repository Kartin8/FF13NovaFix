#pragma once

namespace novafix::game::steam_cloud::shutdown_guard {

using SteamShutdownFn = void (__cdecl*)();

void Configure(SteamShutdownFn original);
void TrackStatsReceivedCallback(void* callback);
void CallbackUnregistered(void* callback);
void __cdecl ProtectedShutdown();

} // namespace novafix::game::steam_cloud::shutdown_guard
