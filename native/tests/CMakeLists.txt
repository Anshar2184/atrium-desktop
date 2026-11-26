add_executable(display_settings_tests display_settings.cpp)
target_link_libraries(display_settings_tests PRIVATE atrium_core_lib)
add_test(NAME display_settings COMMAND display_settings_tests --development)

add_executable(app_processes_tests app_processes.cpp)
target_link_libraries(app_processes_tests PRIVATE atrium_core_lib)
add_test(NAME app_processes COMMAND app_processes_tests)
set_tests_properties(app_processes PROPERTIES TIMEOUT 20)

add_executable(bar_edge_tests bar_edge.cpp)
target_link_libraries(bar_edge_tests PRIVATE atrium_core_lib)
add_test(NAME bar_edge COMMAND bar_edge_tests)

add_executable(capture_policy_tests capture_policy.cpp)
target_link_libraries(capture_policy_tests PRIVATE atrium_core_lib)
add_test(NAME capture_policy COMMAND capture_policy_tests)

add_executable(near_hover_tests near_hover.cpp)
target_link_libraries(near_hover_tests PRIVATE atrium_core_lib)
add_test(NAME near_hover COMMAND near_hover_tests)

add_executable(hover_wave_tests hover_wave.cpp)
target_link_libraries(hover_wave_tests PRIVATE atrium_core_lib)
add_test(NAME hover_wave COMMAND hover_wave_tests)

add_executable(stage_excluded_tests stage_excluded.cpp)
target_link_libraries(stage_excluded_tests PRIVATE atrium_core_lib)
add_test(NAME stage_excluded COMMAND stage_excluded_tests)

add_executable(tray_interaction_tests tray_interaction.cpp)
target_link_libraries(tray_interaction_tests PRIVATE atrium_core_lib)
add_test(NAME tray_interaction COMMAND tray_interaction_tests)
set_tests_properties(tray_interaction PROPERTIES TIMEOUT 20)

add_executable(quiet_profile_tests quiet_profile.cpp)
target_link_libraries(quiet_profile_tests PRIVATE atrium_core_lib)
add_test(NAME quiet_profile COMMAND quiet_profile_tests)

add_executable(quick_toggles_tests quick_toggles.cpp)
target_link_libraries(quick_toggles_tests PRIVATE atrium_core_lib)
add_test(NAME quick_toggles COMMAND quick_toggles_tests)

add_executable(usb_volume_tests usb_volume.cpp)
target_link_libraries(usb_volume_tests PRIVATE atrium_core_lib)
add_test(NAME usb_volume COMMAND usb_volume_tests)
set_tests_properties(usb_volume PROPERTIES TIMEOUT 20)

add_executable(localization_tests localization_checks.cpp)
target_link_libraries(localization_tests PRIVATE atrium_core_lib)
add_test(NAME localization COMMAND localization_tests)
add_custom_command(TARGET localization_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/../locales" "$<TARGET_FILE_DIR:localization_tests>/locales"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/../tests/fixtures/locales" "$<TARGET_FILE_DIR:localization_tests>/locales")

add_executable(tray_match_tests tray_match.cpp)
target_link_libraries(tray_match_tests PRIVATE atrium_core_lib)
add_test(NAME tray_match COMMAND tray_match_tests)

add_executable(tray_click_tests tray_click.cpp)
target_link_libraries(tray_click_tests PRIVATE atrium_core_lib)
add_test(NAME tray_click COMMAND tray_click_tests)

add_executable(tray_notify_probe tray_notify_probe.cpp)
target_link_libraries(tray_notify_probe PRIVATE atrium_core_lib)

# Manual, read-only by default. Explicit --click uses the actual TrayModel worker.
add_executable(tray_live_probe tray_live_probe.cpp)
target_link_libraries(tray_live_probe PRIVATE atrium_core_lib)

add_executable(tray_protocol_tests tray_protocol.cpp)
target_link_libraries(tray_protocol_tests PRIVATE atrium_core_lib)
add_test(NAME tray_protocol COMMAND tray_protocol_tests)

# Puts a tray host in front of Explorer's for a few seconds: run by hand, with Atrium's host off.
add_executable(tray_host_probe tray_host_probe.cpp)
target_link_libraries(tray_host_probe PRIVATE atrium_core_lib)
add_executable(stage_scroll_tests stage_scroll.cpp)
target_link_libraries(stage_scroll_tests PRIVATE atrium_core_lib)
add_test(NAME stage_scroll COMMAND stage_scroll_tests)

add_executable(messengers_tests messengers.cpp)
target_link_libraries(messengers_tests PRIVATE atrium_core_lib)
add_test(NAME messengers COMMAND messengers_tests)

add_executable(ime_status_tests ime_status.cpp)
target_link_libraries(ime_status_tests PRIVATE atrium_core_lib)
add_test(NAME ime_status COMMAND ime_status_tests)

add_executable(network_monitor_tests network_monitor.cpp)
target_link_libraries(network_monitor_tests PRIVATE atrium_core_lib)
add_test(NAME network_monitor COMMAND network_monitor_tests)

add_executable(panel_fixes_tests panel_fixes.cpp)
target_link_libraries(panel_fixes_tests PRIVATE atrium_core_lib)
add_test(NAME panel_fixes COMMAND panel_fixes_tests)

add_executable(win_key_tests win_key.cpp)
target_link_libraries(win_key_tests PRIVATE atrium_core_lib)
add_test(NAME win_key COMMAND win_key_tests)

add_executable(win32_path_tests win32_path.cpp)
target_link_libraries(win32_path_tests PRIVATE atrium_core_lib)
add_test(NAME win32_path COMMAND win32_path_tests)

add_executable(tray_host_exit_tests tray_host_exit.cpp)
target_link_libraries(tray_host_exit_tests PRIVATE atrium_core_lib)
add_test(NAME tray_host_exit COMMAND tray_host_exit_tests)
set_tests_properties(tray_host_exit PROPERTIES TIMEOUT 15)

add_executable(weather_tests weather.cpp)
target_link_libraries(weather_tests PRIVATE atrium_core_lib)
add_test(NAME weather COMMAND weather_tests)
set_tests_properties(weather PROPERTIES TIMEOUT 20)

add_executable(shell_taskbar_tests shell_taskbar.cpp)
target_link_libraries(shell_taskbar_tests PRIVATE atrium_core_lib)
add_test(NAME shell_taskbar COMMAND shell_taskbar_tests)
set_tests_properties(shell_taskbar PROPERTIES TIMEOUT 15)

add_executable(parsers_tests parsers.cpp)
target_link_libraries(parsers_tests PRIVATE atrium_core_lib)
add_test(NAME parsers COMMAND parsers_tests)

add_executable(core_exit_tests core_exit.cpp)
target_link_libraries(core_exit_tests PRIVATE atrium_core_lib)
add_test(NAME core_exit COMMAND core_exit_tests)
set_tests_properties(core_exit PROPERTIES TIMEOUT 15)

add_executable(bar_shutdown_tests bar_shutdown.cpp)
target_link_libraries(bar_shutdown_tests PRIVATE atrium_core_lib)
add_test(NAME bar_shutdown COMMAND bar_shutdown_tests)
set_tests_properties(bar_shutdown PROPERTIES TIMEOUT 15)

add_executable(persistence_recovery_tests persistence_recovery.cpp)
target_link_libraries(persistence_recovery_tests PRIVATE atrium_core_lib)
add_test(NAME persistence_recovery COMMAND persistence_recovery_tests)

add_executable(settings_server_tests settings_server.cpp)
target_link_libraries(settings_server_tests PRIVATE atrium_core_lib)
add_test(NAME settings_server COMMAND settings_server_tests)
set_tests_properties(settings_server PROPERTIES TIMEOUT 20)

add_executable(launchpad_model_tests launchpad_model.cpp)
target_link_libraries(launchpad_model_tests PRIVATE atrium_core_lib)
add_test(NAME launchpad_model COMMAND launchpad_model_tests)

add_executable(media_model_tests media_model.cpp)
target_link_libraries(media_model_tests PRIVATE atrium_core_lib)
add_test(NAME media_model COMMAND media_model_tests)

add_executable(folder_stacks_tests folder_stacks.cpp)
target_link_libraries(folder_stacks_tests PRIVATE atrium_core_lib)
add_test(NAME folder_stacks COMMAND folder_stacks_tests)
set_tests_properties(folder_stacks PROPERTIES TIMEOUT 15)

add_executable(music_bars_tests music_bars.cpp)
target_link_libraries(music_bars_tests PRIVATE atrium_core_lib)
add_test(NAME music_bars COMMAND music_bars_tests)

add_executable(folder_stack_layout_tests folder_stack_layout.cpp)
target_link_libraries(folder_stack_layout_tests PRIVATE atrium_core_lib)
add_test(NAME folder_stack_layout COMMAND folder_stack_layout_tests)

add_executable(ime_glyph_tests ime_glyph.cpp)
target_link_libraries(ime_glyph_tests PRIVATE atrium_core_lib)
add_test(NAME ime_glyph COMMAND ime_glyph_tests)

# The bubble's model is pure: no window, no device. The check builds only it
# and the test, so it runs anywhere.
add_executable(ime_bubble_tests ime_bubble.cpp ../core/ui/ime_bubble_model.cpp)
target_include_directories(ime_bubble_tests PRIVATE ../core)
target_compile_definitions(ime_bubble_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME ime_bubble COMMAND ime_bubble_tests)

add_executable(start_menu_sort_tests start_menu_sort.cpp)
target_link_libraries(start_menu_sort_tests PRIVATE atrium_core_lib)
add_test(NAME start_menu_sort COMMAND start_menu_sort_tests)

add_executable(hibernate_status_tests hibernate_status.cpp)
target_link_libraries(hibernate_status_tests PRIVATE atrium_core_lib)
add_test(NAME hibernate_status COMMAND hibernate_status_tests)

add_executable(power_card_hit_tests power_card_hit.cpp)
target_link_libraries(power_card_hit_tests PRIVATE atrium_core_lib)
add_test(NAME power_card_hit COMMAND power_card_hit_tests)

add_executable(widget_refresh_tests widget_refresh.cpp)
target_link_libraries(widget_refresh_tests PRIVATE atrium_core_lib)
add_test(NAME widget_refresh COMMAND widget_refresh_tests)

add_executable(icon_cache_tests icon_cache_tests.cpp)
target_link_libraries(icon_cache_tests PRIVATE atrium_core_lib)
add_test(NAME icon_cache COMMAND icon_cache_tests)

add_executable(search_query_tests search_query_tests.cpp)
target_link_libraries(search_query_tests PRIVATE atrium_core_lib)
add_test(NAME search_query COMMAND search_query_tests)

add_executable(panel_capture_crop_tests panel_capture_crop.cpp)
target_link_libraries(panel_capture_crop_tests PRIVATE atrium_core_lib)
add_test(NAME panel_capture_crop COMMAND panel_capture_crop_tests)

add_executable(desktop_backdrop_target_tests desktop_backdrop_target.cpp)
target_link_libraries(desktop_backdrop_target_tests PRIVATE atrium_core_lib)
add_test(NAME desktop_backdrop_target COMMAND desktop_backdrop_target_tests)

add_executable(window_snapshot_tests window_snapshot.cpp)
target_link_libraries(window_snapshot_tests PRIVATE atrium_core_lib)
add_test(NAME window_snapshot COMMAND window_snapshot_tests)

add_executable(crash_dump_tests crash_dump.cpp)
target_link_libraries(crash_dump_tests PRIVATE atrium_core_lib)
add_test(NAME crash_dump COMMAND crash_dump_tests --development --data-dir "${CMAKE_CURRENT_BINARY_DIR}/crash_dump_test_data")
set_tests_properties(crash_dump PROPERTIES TIMEOUT 30)

add_executable(glass_present_retry_tests glass_present_retry.cpp)
target_link_libraries(glass_present_retry_tests PRIVATE atrium_core_lib)
add_test(NAME glass_present_retry COMMAND glass_present_retry_tests)

add_executable(stage_watch_tests stage_watch.cpp)
target_link_libraries(stage_watch_tests PRIVATE atrium_core_lib)
add_test(NAME stage_watch COMMAND stage_watch_tests)

add_executable(app_catalog_rules_tests app_catalog_rules.cpp)
target_link_libraries(app_catalog_rules_tests PRIVATE atrium_core_lib)
add_test(NAME app_catalog_rules COMMAND app_catalog_rules_tests)
