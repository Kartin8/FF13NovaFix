#pragma once

#include "game/titles/ff13_2/rendering/shadows/alias_transaction.h"
#include "game/titles/ff13_2/rendering/shadows/resource_snapshot.h"

#include <cstdint>
#include <optional>

namespace novafix::game::xiii2_shadow_frame_runtime {

struct FrameToken {
    bool active{};
    void* renderer{};
    const xiii2_shadow_resources::Snapshot* snapshot{};
};

void BindResourceSlots(void** deviceSlot, void** currentColorOwnerSlot,
                       void** currentDepthOwnerSlot);
void SetHooksReady(bool ready);
bool HooksReady();

void FailClosed(const char* reason);
void Activate();
bool Enabled();
bool FailedClosed();

FrameToken BeginFrame(
    void* renderer, xiii2_shadow_resources::Snapshot& snapshot,
    xiii2_shadow_alias::AliasTransaction& transaction);
void ApplyReducedFarReceiverMatrix(
    void* renderer, const xiii2_shadow_resources::Snapshot& snapshot);
void RestoreFrame(const FrameToken& previous);

const xiii2_shadow_resources::Snapshot* CurrentSnapshot();
std::optional<std::uint32_t> ActiveCascade();
bool SuppressNativePacking(void* source, void* destination);

} // namespace novafix::game::xiii2_shadow_frame_runtime
