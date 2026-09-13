cmake_minimum_required(VERSION 3.21)
project(NovaFix VERSION 1.0.0 LANGUAGES C CXX RC)

option(NOVAFIX_BUILD_INTERNAL_TESTS
       "Build NovaFix internal policy and filesystem tests" OFF)

if(NOT WIN32)
    message(FATAL_ERROR "NovaFix must be built for a Windows target")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "NovaFix must be built for x86")
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Supported compilers are MSVC and LLVM-MinGW Clang")
endif()

add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)
configure_file(src/novafix.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/novafix.rc" @ONLY)
set(EXPORT_SYMBOL_PREFIX "")
if(MSVC)
    set(EXPORT_SYMBOL_PREFIX "_")
endif()
configure_file(src/proxy_exports.def.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/proxy_exports.def" @ONLY)

set(NOVAFIX_SOURCES
    src/d3d9/alpha_shadow_variants.cpp
    src/diagnostics/hook_profiler.cpp
    src/diagnostics/log.cpp
    src/common/module_path.cpp
    src/compat/addons/addon_loader.cpp
    src/compat/addons/addon_profile_storage.cpp
    src/compat/addons/addon_storage.cpp
    src/compat/addons/native_notifications.cpp
    src/compat/addons/plugin_controls.cpp
    src/compat/addons/plugin_descriptor.cpp
    src/compat/compatibility.cpp
    src/compat/modules/foreign_fix.cpp
    src/compat/modules/loaded_modules.cpp
    src/compat/platform.cpp
    src/d3d9/backend.cpp
    src/d3d9/device_hook_policy.cpp
    src/d3d9/device_hooks.cpp
    src/d3d9/device_shader_hooks.cpp
    src/d3d9/device_state_hooks.cpp
    src/d3d9/factory_hooks.cpp
    src/d3d9/mip_lod_bias_controller.cpp
    src/d3d9/presentation_setup.cpp
    src/d3d9/redundant_state_filter.cpp
    src/d3d9/shader_bytecode_identity.cpp
    src/d3d9/shader_corrections.cpp
    src/d3d9/presentation_router.cpp
    src/d3d9/swap_chain_proxy.cpp
    src/diagnostics/crash_dumps.cpp
    src/display/display_profile.cpp
    src/display/dpi_awareness.cpp
    src/display/monitor_layout.cpp
    src/display/presentation_policy.cpp
    src/display/scissor_transform.cpp
    src/display/texture_policy.cpp
    src/display/ui_render_policy.cpp
    src/display/window_geometry.cpp
    src/game/core/catalog.cpp
    src/game/core/executable_fingerprint.cpp
    src/game/core/features.cpp
    src/game/core/game_profile.cpp
    src/game/core/image_view.cpp
    src/game/core/release_channel.cpp
    src/game/shared/controllers/controller_hotplug.cpp
    src/game/shared/controllers/controller_hotplug_backend.cpp
    src/game/shared/controllers/controller_vibration.cpp
    src/game/shared/timing/cutscene_terminal_frame_runtime.cpp
    src/game/shared/timing/frame_pacing.cpp
    src/game/shared/graceful_shutdown.cpp
    src/game/shared/timing/facial_animation_timing.cpp
    src/game/shared/timing/facial_animation_policy.cpp
    src/game/shared/ui/scissor_fix.cpp
    src/game/shared/ui/startup_module_notification.cpp
    src/game/steam/steam_achievement_queue.cpp
    src/game/steam/steam_keyboard.cpp
    src/game/steam/steam_pending_journals.cpp
    src/game/steam/steam_cloud_safety.cpp
    src/game/steam/steam_remote_storage_catalog.cpp
    src/game/steam/steam_remote_storage_proxy.cpp
    src/game/steam/steam_remote_storage_worker.cpp
    src/game/steam/steam_save_checkpoints.cpp
    src/game/steam/steam_save_files.cpp
    src/game/steam/steam_save_identity.cpp
    src/game/steam/steam_save_mirror.cpp
    src/game/steam/steam_save_sync_state.cpp
    src/game/steam/steam_shutdown_guard.cpp
    src/game/titles/ff13/title_adapter.cpp
    src/game/titles/ff13/animation/motion_blend_correction.cpp
    src/game/titles/ff13/timing/cutscene_terminal_frame.cpp
    src/game/titles/ff13/timing/frame_pacing_runtime.cpp
    src/game/titles/ff13/rendering/nautilus_crash_fix.cpp
    src/game/titles/ff13/ui/prompt_layout_registry.cpp
    src/game/titles/ff13/ui/prompt_mode_manager.cpp
    src/game/titles/ff13/titan_route_correction.cpp
    src/game/titles/ff13/ui/addon_notification.cpp
    src/game/titles/ff13_2/title_adapter.cpp
    src/game/titles/ff13_2/animation/chain_timing.cpp
    src/game/titles/ff13_2/animation/root_motion.cpp
    src/game/titles/ff13_2/camera/chocobo_race.cpp
    src/game/titles/ff13_2/camera/scene_a_bahamut.cpp
    src/game/titles/ff13_2/rendering/hud_sampling.cpp
    src/game/titles/ff13_2/rendering/shadows/alias_transaction.cpp
    src/game/titles/ff13_2/rendering/shadows/atlas_policy.cpp
    src/game/titles/ff13_2/rendering/shadows/direct_atlas.cpp
    src/game/titles/ff13_2/rendering/shadows/fixed_depth.cpp
    src/game/titles/ff13_2/rendering/shadows/foreign_resource_access.cpp
    src/game/titles/ff13_2/rendering/shadows/frame_runtime.cpp
    src/game/titles/ff13_2/rendering/shadows/native_hooks.cpp
    src/game/titles/ff13_2/rendering/shadows/optimization.cpp
    src/game/titles/ff13_2/rendering/shadows/resource_snapshot.cpp
    src/game/titles/ff13_2/resources/budget.cpp
    src/game/titles/ff13_2/resources/unwrap.cpp
    src/game/titles/ff13_2/resources/unwrap_policy.cpp
    src/game/titles/ff13_2/timing/delta_guard.cpp
    src/game/titles/ff13_2/timing/frame_timing_policy.cpp
    src/game/titles/ff13_2/timing/frame_pacing_runtime.cpp
    src/game/titles/ff13_2/timing/cutscene_terminal_frame.cpp
    src/game/titles/ff13_2/ui/prompt_layout_registry.cpp
    src/game/titles/ff13_2/ui/prompt_mode_manager.cpp
    src/game/titles/ff13_2/ui/qte/qte_prompt_lifecycle.cpp
    src/game/titles/ff13_2/ui/qte/qte_visual_bundles.cpp
    src/game/titles/ff13_2/ui/addon_notification.cpp
    src/game/titles/lightning_returns/title_adapter.cpp
    src/game/titles/lightning_returns/naming/chocobo_name.cpp
    src/game/titles/lightning_returns/naming/map_marker_name.cpp
    src/game/titles/lightning_returns/naming/schemata_name.cpp
    src/game/titles/lightning_returns/snapshot/capture_pipeline.cpp
    src/game/titles/lightning_returns/snapshot/image_capture.cpp
    src/game/titles/lightning_returns/snapshot/message_editor_redirect.cpp
    src/game/titles/lightning_returns/snapshot/mode_controller.cpp
    src/game/titles/lightning_returns/snapshot/outerworld_menu.cpp
    src/game/titles/lightning_returns/ui/window_menu_win32.cpp
    src/game/titles/lightning_returns/ui/button_guides/registry.cpp
    src/game/titles/lightning_returns/ui/button_guides/snapshot_hud_guide.cpp
    src/game/titles/lightning_returns/ui/prompt_mode_manager.cpp
    src/game/titles/lightning_returns/ui/message_window.cpp
    src/game/titles/lightning_returns/ui/addon_notification.cpp
    src/game/titles/lightning_returns/ui/text_editor/editor.cpp
    src/game/titles/lightning_returns/ui/text_editor/game_text_converter.cpp
    src/game/titles/lightning_returns/ui/text_editor/input_handler.cpp
    src/game/titles/lightning_returns/ui/text_editor/message_window_adapter.cpp
    src/settings/graphics_settings.cpp
    src/settings/config_path.cpp
    src/settings/default_config.cpp
    src/settings/input_settings.cpp
    src/settings/ini_value.cpp
    src/settings/performance_settings.cpp
    src/input/activity.cpp
    src/input/axis_activity.cpp
    src/input/direct_input/direct_input.cpp
    src/input/direct_input/direct_input_diagnostics.cpp
    src/input/direct_input/direct_input_neutral.cpp
    src/input/direct_input/direct_input_state.cpp
    src/input/hotplug_gate.cpp
    src/overlay/input/overlay_hotkey.cpp
    src/overlay/input/overlay_gamepad.cpp
    src/input/prompt_policy.cpp
    src/input/vibration_envelope.cpp
    src/input/xinput_backend.cpp
    src/input/win32_input.cpp
    src/launcher/command_line.cpp
    src/launcher/command_line_rewriter.cpp
    src/overlay/overlay.cpp
    src/overlay/overlay_style.cpp
    src/overlay/settings_window.cpp
    src/patch/detour32.cpp
    src/patch/iat_hook.cpp
    src/patch/integrity_monitor.cpp
    src/patch/memory_access.cpp
    src/patch/pointer_hook.cpp
    src/patch/relative_branch.cpp
    src/patch/registry.cpp
    src/patch/signature.cpp
    src/patch/transaction.cpp
    src/performance/frame_limiter.cpp
    src/performance/frame_metrics.cpp
    src/performance/frame_schedule.cpp
    src/performance/frame_statistics.cpp
    src/performance/scheduling_stability.cpp
    src/runtime/restart.cpp
    src/runtime/early_d3d_dispatch.cpp
    src/runtime/bootstrap.cpp
    src/overlay/panels/graphics_panel.cpp
    src/overlay/panels/compatibility_panel.cpp
    src/overlay/panels/diagnostics_panel.cpp
    src/overlay/panels/input_panel.cpp
    src/overlay/panels/patch_status_panel.cpp
    src/overlay/panels/performance_panel.cpp
    src/overlay/panels/plugin_panel.cpp
    src/overlay/panels/settings_widgets.cpp
    src/dllmain.cpp
    src/proxy_exports.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/novafix.rc")

if(MSVC)
    list(APPEND NOVAFIX_SOURCES src/common/guarded_call.cpp)
else()
    # Keep x86 SEH in one MSVC-ABI object; the rest remains GNU ABI.
    get_filename_component(LLVM_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(LLVM_MINGW_HOST_CLANGXX
        NAMES clang++ clang++.exe
        HINTS "${LLVM_MINGW_BIN_DIR}"
        NO_DEFAULT_PATH
        REQUIRED)
    set(GUARDED_CALL_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/guarded_call.obj")
    add_custom_command(
        OUTPUT "${GUARDED_CALL_OBJECT}"
        COMMAND "${LLVM_MINGW_HOST_CLANGXX}"
                --target=i686-pc-windows-msvc
                -O2 -fno-exceptions -fno-rtti
                -c "${CMAKE_CURRENT_SOURCE_DIR}/src/common/guarded_call.cpp"
                -o "${GUARDED_CALL_OBJECT}"
        DEPENDS src/common/guarded_call.cpp
        VERBATIM)
    set_source_files_properties("${GUARDED_CALL_OBJECT}"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND NOVAFIX_SOURCES "${GUARDED_CALL_OBJECT}")
endif()

set(IMGUI_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx9.cpp)

add_library(d3d9 SHARED ${NOVAFIX_SOURCES} ${IMGUI_SOURCES})
target_sources(d3d9 PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/proxy_exports.def")
target_include_directories(d3d9 PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    include
    src
    third_party/imgui
    third_party/imgui/backends)
target_compile_features(d3d9 PRIVATE cxx_std_20)
target_compile_definitions(d3d9 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0602
    _WIN32_WINNT=0x0602
    UNICODE
    _UNICODE
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
if(MSVC)
    target_compile_options(d3d9 PRIVATE
        /W4 /WX /EHsc /permissive-
        $<$<CONFIG:Release>:/Gw>
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/GL>)
else()
    target_compile_options(d3d9 PRIVATE
        -Wall -Wextra -Werror
        -Wno-unknown-pragmas
        -fms-extensions)
endif()

target_link_libraries(d3d9 PRIVATE
    bcrypt dwmapi dxguid ole32 user32 version windowscodecs minhook)
if(MSVC)
    target_link_options(d3d9 PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/LTCG>)
else()
    target_link_options(d3d9 PRIVATE -static -Wl,--strip-all)
endif()
set_target_properties(d3d9 PROPERTIES OUTPUT_NAME d3d9 PREFIX "")
