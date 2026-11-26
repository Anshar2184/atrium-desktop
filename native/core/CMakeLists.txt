# Atrium: resident process (glass bar, tray icon, settings pipe).

# HLSL is compiled at build time into headers; the runtime needs no compiler.
find_program(ATRIUM_FXC fxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    REQUIRED)

set(shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders)
set(shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${shader_out})
set(shader_headers)
function(atrium_shader source profile entry variable header)
    add_custom_command(
        OUTPUT ${shader_out}/${header}
        COMMAND ${ATRIUM_FXC} /nologo /O3 /WX /T ${profile} /E ${entry} /Vn ${variable} /Fh ${shader_out}/${header} ${shader_dir}/${source}
        DEPENDS ${shader_dir}/${source}
        COMMENT "fxc ${source} ${entry}"
        VERBATIM)
    set(shader_headers ${shader_headers} ${shader_out}/${header} PARENT_SCOPE)
endfunction()
atrium_shader(glass.hlsl vs_5_0 VS g_GlassVS glass_vs.h)
atrium_shader(glass.hlsl ps_5_0 PS g_GlassPS glass_ps.h)
atrium_shader(glass.hlsl ps_5_0 PSBasic g_GlassBasicPS glass_basic_ps.h)
atrium_shader(compare.hlsl cs_5_0 CS g_CompareCS compare_cs.h)
atrium_shader(backdrop_edge.hlsl ps_5_0 PS g_BackdropEdgePS backdrop_edge_ps.h)
add_custom_target(atrium_shaders DEPENDS ${shader_headers})

add_library(atrium_core_lib STATIC
    app_menus.cpp
    bar.cpp
    bar_drop.cpp
    bar_entries.cpp
    bar_input.cpp
    bar_layout.cpp
    bar_render.cpp
    bar_stacking.cpp
    crash_dump.cpp
    crash_report.cpp
    desktop_helpers.cpp
    notice_actions.cpp
    extras_host.cpp
    guard_process.cpp
    near_hover.cpp
    log.cpp
    settings.cpp
    settings_io.cpp
    settings_server.cpp
    localization.cpp
    shell/ime_status.cpp
    shell/file_box_watch.cpp
    shell/keyboard_hook.cpp
    shell/shell_icon.cpp
    shell/task_launch.cpp
    shell/task_model.cpp
    shell/app_processes.cpp
    shell/app_catalog.cpp
    shell/start_pins.cpp
    shell/start_layout.cpp
    shell/win_key.cpp
    shell/app_registration.cpp
    shell/update_checker.cpp
    shell/system_chrome.cpp
    shell/system_status.cpp
    shell/wifi.cpp
    shell/radios.cpp
    shell/lunar.cpp
    shell/audio_devices.cpp
    shell/audio_meter.cpp
    shell/media_app.cpp
    shell/media_players.cpp
    shell/media_session.cpp
    shell/netease_memory.cpp
    shell/netease_tracks.cpp
    shell/projection.cpp
    shell/quick_links.cpp
    shell/quick_toggles.cpp
    shell/games.cpp
    shell/launchpad_order.cpp
    shell/displays.cpp
    shell/tray_host.cpp
    shell/tray_model.cpp
    shell/tray_match.cpp
    shell/shell_verbs.cpp
    shell/jump_list.cpp
    shell/folder_stacks.cpp
    shell/wechat_source.cpp
    shell/wechat_open.cpp
    shell/feishu_source.cpp
    ui/surface.cpp
    ui/popup_theme.cpp
    ui/canvas.cpp
    ui/start_menu.cpp
    ui/start_menu_input.cpp
    ui/start_menu_menus.cpp
    ui/notifications.cpp
    ui/notification_popup.cpp
    ui/tray_panel.cpp
    ui/calendar_panel.cpp
    ui/control_center.cpp
    ui/control_center_paint.cpp
    ui/control_modules.cpp
    ui/desktop_widgets.cpp
    ui/widget_stacking.cpp
    ui/folder_stack_popup.cpp
    ui/genie_minimize.cpp
    ui/glass_menu.cpp
    ui/ime_bubble.cpp
    ui/ime_bubble_model.cpp
    ui/launchpad.cpp
    ui/launchpad_input.cpp
    ui/launchpad_drag.cpp
    ui/now_playing_card.cpp
    ui/stage_manager.cpp
    ui/stage_drag.cpp
    ui/stage_close.cpp
    ui/stage_paint.cpp
    ui/stage_restore.cpp
    ui/stage_fade.cpp
    ui/window_snapshot.cpp
    shell/weather.cpp
    shell/bluetooth_batteries.cpp
    shell/network_monitor.cpp
    shell/logon_task.cpp
    ui/window_peek.cpp
    ui/window_preview.cpp
    ui/widget_faces.cpp
    shell/notification_listener.cpp
    shell/usb_volume.cpp
    render/bar_painter.cpp
    render/bar_item_painter.cpp
    render/bar_extras_painter.cpp
    render/ime_glyph.cpp
    render/hover_wave.cpp
    render/capture.cpp
    render/window_capture.cpp
    render/desktop_backdrop.cpp
    render/glass_renderer.cpp
    render/glass_panel.cpp
    render/panel_glass_host.cpp
    render/ui_font.cpp
    render/gpu.cpp
    render/wifi_glyph.cpp)
add_dependencies(atrium_core_lib atrium_shaders)
# C++/WinRT headers come with the Windows SDK.
target_include_directories(atrium_core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${shader_out})
target_compile_definitions(atrium_core_lib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
if(ATRIUM_DEVELOPMENT)
    target_compile_definitions(atrium_core_lib PUBLIC ATRIUM_DEVELOPMENT)
endif()
target_link_libraries(atrium_core_lib PUBLIC
    windowsapp d3d11 dxgi dxguid dcomp d2d1 dwrite shcore user32 gdi32 shell32 ole32
    dwmapi version propsys shlwapi oleaut32 uuid imm32 powrprof wlanapi windowscodecs winhttp setupapi taskschd
    iphlpapi advapi32 comctl32 icu wbemuuid secur32 winsqlite3)

add_executable(atrium_core WIN32 main.cpp core.rc core.manifest)
# build/win-x64/<Config>/Atrium.exe
set_target_properties(atrium_core PROPERTIES OUTPUT_NAME "Atrium" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# The interface face (D40) next to the exe, as in the package.
set(ui_fonts MiSans-Medium.otf MiSans-Demibold.otf MiSans-Semibold.otf)
list(TRANSFORM ui_fonts PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/fonts/)
add_custom_command(TARGET atrium_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:atrium_core>/Fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ui_fonts} $<TARGET_FILE_DIR:atrium_core>/Fonts
    VERBATIM)
target_link_libraries(atrium_core PRIVATE atrium_core_lib)
