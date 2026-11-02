# Atrium.Core: resident process (glass bar, tray icon, settings pipe).

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
atrium_shader(compare.hlsl cs_5_0 CS g_CompareCS compare_cs.h)

add_executable(atrium_core WIN32
    main.cpp
    bar.cpp
    log.cpp
    settings.cpp
    settings_server.cpp
    shell/task_model.cpp
    shell/app_catalog.cpp
    shell/system_status.cpp
    shell/wifi.cpp
    shell/radios.cpp
    shell/lunar.cpp
    shell/audio_devices.cpp
    shell/projection.cpp
    shell/tray_model.cpp
    shell/wechat_source.cpp
    shell/feishu_source.cpp
    ui/surface.cpp
    ui/start_menu.cpp
    ui/notifications.cpp
    ui/tray_panel.cpp
    ui/calendar_panel.cpp
    ui/lock_screen.cpp
    ui/control_center.cpp
    ui/desktop_widgets.cpp
    shell/notification_listener.cpp
    render/bar_painter.cpp
    render/capture.cpp
    render/glass_renderer.cpp
    render/glass_panel.cpp
    render/gpu.cpp
    core.rc
    core.manifest
    ${shader_headers})
# Next to Atrium.Guard.exe: build/win-x64/<Config>/
set_target_properties(atrium_core PROPERTIES OUTPUT_NAME "Atrium.Core" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_include_directories(atrium_core PRIVATE ${shader_out})
# C++/WinRT headers come with the Windows SDK.
target_compile_options(atrium_core PRIVATE /bigobj)
target_compile_definitions(atrium_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(atrium_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# shell/task_model.cpp: DWM cloaking, AUMID property store, file version info, shell item images.
set(atrium_task_libs windowsapp dwmapi version propsys shlwapi ole32 oleaut32 uuid shell32 user32 gdi32)
target_link_libraries(atrium_core PRIVATE
    windowsapp d3d11 dxgi dcomp d2d1 dwrite shcore user32 gdi32 shell32 ole32
    dwmapi version propsys shlwapi oleaut32 uuid imm32 powrprof wlanapi windowscodecs)

# Console probe for the task model (not part of the default build):
#   atrium_task_probe.exe <seconds> <icon-dir> [--selftest]
add_executable(atrium_task_probe EXCLUDE_FROM_ALL
    shell/task_model_probe.cpp
    shell/task_model.cpp
    log.cpp)
set_target_properties(atrium_task_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_include_directories(atrium_task_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(atrium_task_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(atrium_task_probe PRIVATE ${atrium_task_libs} windowscodecs)
