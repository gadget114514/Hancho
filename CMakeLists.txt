cmake_minimum_required(VERSION 3.20)
project(Hancho VERSION 0.9.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the MSVC runtime statically so the executables run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DECODE_ENABLE_SIXEL)

# Version macros for C++ (PC_VERSION) and the VERSIONINFO resource (src/version_info.rc2).
set(PC_VERSION_DEFINITIONS
    PC_VERSION="${PROJECT_VERSION}"
    PC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PC_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PC_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)


if(WIN32)

add_executable(Hancho
    src/main.cpp
    src/common.cpp
    src/platform/platform_windows.cpp
    src/process_manager.cpp
    src/taskqueue.cpp
    src/chain_guard.cpp
    src/ipc_server.cpp
    src/ipc_server_remote.cpp
    src/webui_server.cpp
    src/webui_assets.cpp
    src/ipc_client.cpp
    src/remote_client.cpp
    src/remote_hosts.cpp
    src/cli.cpp
    src/memory_cli.cpp
    src/mcp_server.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/memory_search.cpp
    src/memory_record.cpp
    src/activity_tree.cpp
    src/bt_debug_view.cpp
    src/behavior_tree.cpp
    src/behavior_tree_parse.cpp
    src/behavior_tree_vars.cpp
    src/behavior_tree_cond.cpp
    src/behavior_tree_exec.cpp
    src/behavior_tree_escalate.cpp
    src/behavior_tree_state.cpp
    src/behavior_tree_runtime.cpp
    src/worker_engines.cpp
    src/bt_manager.cpp
    src/bt_manager_api.cpp
    src/gui_dashboard.cpp
    src/app.rc
    src/agent_template.rc
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
    src/Terminal/src/TerminalGridView.cpp
)

target_compile_definitions(Hancho PRIVATE ${PC_VERSION_DEFINITIONS})

target_include_directories(Hancho PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(Hancho PRIVATE /W4 /utf-8 /EHsc)
    target_link_options(Hancho PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()

target_link_libraries(Hancho PRIVATE
    user32
    gdi32
    comctl32
    comdlg32
    uxtheme
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    d2d1
    dwrite
    dwmapi
    windowscodecs
    crypt32
    ws2_32
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET Hancho POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:Hancho>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:Hancho>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe to output directory"
)

# Console client (human-friendly by default, --json for machine-readable output)
add_executable(hancho-cli
    src/cli_main.cpp
    src/cli.cpp
    src/ipc_client.cpp
    src/remote_client.cpp
    src/remote_hosts.cpp
    src/event_adapter.cpp
    src/memory_cli.cpp
    src/memory_search.cpp
    src/memory_record.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/common.cpp
    src/hancho-cli.rc
    src/agent_template.rc
)

# Rebuild the embedded template when hancho-runner.md changes.
set_source_files_properties(src/agent_template.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/agents/hancho-runner.md")

add_dependencies(hancho-cli Hancho)

target_compile_definitions(hancho-cli PRIVATE ${PC_VERSION_DEFINITIONS})

target_include_directories(hancho-cli PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(hancho-cli PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(hancho-cli PRIVATE
    user32
    shell32
    advapi32
    crypt32
    ws2_32
)

# Headless smoke test for the task queue engine (deps, retries, chain guard, persistence).
add_executable(taskqueue_smoke
    tests/taskqueue_smoke.cpp
    src/taskqueue.cpp
    src/chain_guard.cpp
    src/process_manager.cpp
    src/platform/platform_windows.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/common.cpp
    src/memory_search.cpp
    src/memory_record.cpp
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
)

target_include_directories(taskqueue_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(taskqueue_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(taskqueue_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    d2d1
    dwrite
    dwmapi
    windowscodecs
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET taskqueue_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:taskqueue_smoke>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:taskqueue_smoke>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe for taskqueue_smoke"
)

# Headless smoke test for the LLM stream tracker (Claude sub-agent events, token accounting).
add_executable(llm_stream_smoke
    tests/llm_stream_smoke.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/common.cpp
)

target_compile_definitions(llm_stream_smoke PRIVATE PC_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Headless test for the monitor tree rules (src/activity_tree.cpp).
add_executable(activity_tree_smoke
    tests/activity_tree_smoke.cpp
    src/activity_tree.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/common.cpp
)

target_include_directories(activity_tree_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(activity_tree_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(activity_tree_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
)

target_include_directories(llm_stream_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(llm_stream_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(llm_stream_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

# Headless smoke test for the hancho.event/1 normalizer and the .events.jsonl writer.
add_executable(event_schema_smoke
    tests/event_schema_smoke.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/common.cpp
)

target_compile_definitions(event_schema_smoke PRIVATE PC_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Headless smoke test for the Tier 1 declarative adapters (src/event_adapter.cpp).
add_executable(event_adapter_smoke
    tests/event_adapter_smoke.cpp
    src/event_adapter.cpp
    src/event_schema.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/common.cpp
)

target_include_directories(event_adapter_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(event_adapter_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(event_adapter_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

# Headless smoke test for HANCHO_EVENT_PIPE (src/process_manager.cpp + the event writer).
add_executable(event_pipe_smoke
    tests/event_pipe_smoke.cpp
    src/process_manager.cpp
    src/platform/platform_windows.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/common.cpp
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
)

target_include_directories(event_pipe_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(event_pipe_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(event_pipe_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    d2d1
    dwrite
    dwmapi
    windowscodecs
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET event_pipe_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:event_pipe_smoke>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:event_pipe_smoke>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe for event_pipe_smoke"
)

target_include_directories(event_schema_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(event_schema_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(event_schema_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

# Headless test for the behavior tree debug model (src/bt_debug_view.cpp).
add_executable(bt_debug_view_smoke
    tests/bt_debug_view_smoke.cpp
    src/bt_debug_view.cpp
    src/common.cpp
)

target_compile_definitions(bt_debug_view_smoke PRIVATE PC_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories(bt_debug_view_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(bt_debug_view_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(bt_debug_view_smoke PRIVATE
    user32
    shell32
    advapi32
    crypt32
)

# Headless test for the worker engine registry and output schema validator (src/worker_engines.cpp).
add_executable(worker_engines_smoke
    tests/worker_engines_smoke.cpp
    src/worker_engines.cpp
    src/platform/platform_windows.cpp
    src/common.cpp
)

target_include_directories(worker_engines_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(worker_engines_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(worker_engines_smoke PRIVATE
    user32
    shell32
    advapi32
    psapi
    crypt32
)

# Headless test for the behavior tree core engine (src/behavior_tree*.cpp).
add_executable(behavior_tree_smoke
    tests/behavior_tree_smoke.cpp
    tests/behavior_tree_smoke_part2.cpp
    src/behavior_tree.cpp
    src/behavior_tree_parse.cpp
    src/behavior_tree_vars.cpp
    src/behavior_tree_cond.cpp
    src/behavior_tree_exec.cpp
    src/behavior_tree_escalate.cpp
    src/behavior_tree_state.cpp
    src/behavior_tree_runtime.cpp
    src/worker_engines.cpp
    src/platform/platform_windows.cpp
    src/bt_debug_view.cpp
    src/common.cpp
)

target_compile_definitions(behavior_tree_smoke PRIVATE PC_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories(behavior_tree_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(behavior_tree_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(behavior_tree_smoke PRIVATE
    user32
    shell32
    advapi32
    psapi
    crypt32
)

# Headless integration test for BtManager + TaskQueueManager + ProcessManager.
add_executable(bt_integration_smoke
    tests/bt_integration_smoke.cpp
    tests/bt_integration_smoke_part2.cpp
    src/bt_manager.cpp
    src/bt_manager_api.cpp
    src/behavior_tree.cpp
    src/behavior_tree_parse.cpp
    src/behavior_tree_vars.cpp
    src/behavior_tree_cond.cpp
    src/behavior_tree_exec.cpp
    src/behavior_tree_escalate.cpp
    src/behavior_tree_state.cpp
    src/behavior_tree_runtime.cpp
    src/worker_engines.cpp
    src/platform/platform_windows.cpp
    src/taskqueue.cpp
    src/memory_record.cpp
    src/memory_search.cpp
    src/chain_guard.cpp
    src/process_manager.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/common.cpp
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
)

target_include_directories(bt_integration_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(bt_integration_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(bt_integration_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    d2d1
    dwrite
    dwmapi
    windowscodecs
    crypt32
    ws2_32
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

# Headless smoke test for BT IPC actions (src/ipc_server.cpp + BtManager + TaskQueueManager + ProcessManager).
add_executable(bt_ipc_smoke
    tests/bt_ipc_smoke.cpp
    src/ipc_server.cpp
    src/ipc_server_remote.cpp
    src/webui_server.cpp
    src/webui_assets.cpp
    src/remote_client.cpp
    src/remote_hosts.cpp
    src/bt_manager.cpp
    src/bt_manager_api.cpp
    src/behavior_tree.cpp
    src/behavior_tree_parse.cpp
    src/behavior_tree_vars.cpp
    src/behavior_tree_cond.cpp
    src/behavior_tree_exec.cpp
    src/behavior_tree_escalate.cpp
    src/behavior_tree_state.cpp
    src/behavior_tree_runtime.cpp
    src/worker_engines.cpp
    src/platform/platform_windows.cpp
    src/taskqueue.cpp
    src/memory_record.cpp
    src/memory_search.cpp
    src/chain_guard.cpp
    src/process_manager.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
    src/common.cpp
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
)

target_include_directories(bt_ipc_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(bt_ipc_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(bt_ipc_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    d2d1
    dwrite
    dwmapi
    windowscodecs
    crypt32
    ws2_32
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)




# Headless smoke test for memory search/index/rebuild (src/memory_search.cpp).
add_executable(memory_search_smoke
    tests/memory_search_smoke.cpp
    src/memory_search.cpp
    src/memory_record.cpp
    src/common.cpp
)

target_include_directories(memory_search_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(memory_search_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(memory_search_smoke PRIVATE
    user32
    shell32
    advapi32
    crypt32
)

# Headless smoke test for L1 run records (src/memory_record.cpp).
add_executable(memory_record_smoke
    tests/memory_record_smoke.cpp
    src/memory_record.cpp
    src/common.cpp
)

target_include_directories(memory_record_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(memory_record_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(memory_record_smoke PRIVATE
    user32
    shell32
    advapi32
    crypt32
)

# Smoke test for the `--headless` core: starts the real Hancho.exe without a
# dashboard, drives it over the Named Pipe, reconnects between requests, and
# stops it through IPC.
add_executable(headless_smoke
    tests/headless_smoke.cpp
    src/ipc_client.cpp
    src/common.cpp
)

add_dependencies(headless_smoke Hancho)

target_include_directories(headless_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(headless_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(headless_smoke PRIVATE
    user32
    shell32
    advapi32
    crypt32
)

# Unit smoke test for the platform boundary: shell quoting, instance lock and
# IPC transport through platform/platform.h only.
add_executable(platform_abstraction_smoke
    tests/platform_abstraction_smoke.cpp
    src/platform/platform_windows.cpp
)

target_include_directories(platform_abstraction_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(platform_abstraction_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(platform_abstraction_smoke PRIVATE
    user32
    advapi32
    psapi
)

# Unit / smoke test for GUI platform abstraction layer (tests/gui_platform.h, Card 060d).
add_executable(gui_platform_smoke
    tests/gui_platform_smoke.cpp
)

target_include_directories(gui_platform_smoke PRIVATE
    src
    include
    tests
)

if(MSVC)
    target_compile_options(gui_platform_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(gui_platform_smoke PRIVATE
    user32
    shell32
    advapi32
    comdlg32
    winmm
)

# Smoke test for the hancho-cli top TUI helpers and renderer (Cards 062-1, 062-2).
add_executable(cli_top_smoke
    tests/cli_top_smoke.cpp
    src/common.cpp
    src/ipc_client.cpp
)

target_include_directories(cli_top_smoke PRIVATE
    src
    include
    tests
)

if(MSVC)
    target_compile_options(cli_top_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(cli_top_smoke PRIVATE
    user32
    shell32
    advapi32
    crypt32
    ws2_32
)

# Smoke test for browser UI skeleton (HTTP + WebSocket server, Card 061b).
add_executable(webui_smoke
    tests/webui_smoke.cpp
    src/webui_server.cpp
    src/webui_assets.cpp
    src/remote_client.cpp
    src/remote_hosts.cpp
    src/common.cpp
    src/platform/platform_windows.cpp
)

target_include_directories(webui_smoke PRIVATE
    src
    include
    tests
)

if(MSVC)
    target_compile_options(webui_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(webui_smoke PRIVATE
    ws2_32
    crypt32
    advapi32
    shell32
    user32
)


else()
    # POSIX headless core and CLI (doc/design-v3.md 2.4). There is no GUI here:
    # the only screen is `hancho-cli top`.
    find_package(Threads REQUIRED)

    set(HANCHO_CORE_SOURCES
        src/common_posix.cpp
        src/platform/platform_posix.cpp
        src/process_manager_posix.cpp
        src/taskqueue.cpp
        src/chain_guard_posix.cpp
        src/ipc_server.cpp
        src/ipc_server_posix.cpp
        src/ipc_server_remote.cpp
        src/webui_server.cpp
        src/webui_assets.cpp
        src/ipc_client_posix.cpp
        src/remote_client.cpp
        src/remote_hosts.cpp
        src/mcp_server.cpp
        src/llm_stream.cpp
        src/llm_stream_claude.cpp
        src/llm_stream_opencode.cpp
        src/llm_stream_agy.cpp
        src/llm_stream_codex.cpp
        src/event_schema.cpp
        src/event_adapter.cpp
        src/memory_search.cpp
        src/memory_record.cpp
        src/activity_tree.cpp
        src/bt_debug_view.cpp
        src/behavior_tree.cpp
        src/behavior_tree_parse.cpp
        src/behavior_tree_vars.cpp
        src/behavior_tree_cond.cpp
        src/behavior_tree_exec.cpp
        src/behavior_tree_escalate.cpp
        src/behavior_tree_state.cpp
        src/behavior_tree_runtime.cpp
        src/worker_engines.cpp
        src/bt_manager.cpp
        src/bt_manager_api.cpp
    )

    add_executable(Hancho src/main_posix.cpp ${HANCHO_CORE_SOURCES})
    target_compile_definitions(Hancho PRIVATE ${PC_VERSION_DEFINITIONS})
    target_include_directories(Hancho PRIVATE src include)
    target_compile_options(Hancho PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(Hancho PRIVATE Threads::Threads util)

    add_executable(hancho-cli
        src/cli_main_posix.cpp
        src/cli.cpp
        src/ipc_client_posix.cpp
        src/remote_client.cpp
        src/remote_hosts.cpp
        src/event_adapter.cpp
        src/memory_cli.cpp
        src/memory_search.cpp
        src/memory_record.cpp
        src/llm_stream.cpp
        src/llm_stream_claude.cpp
        src/llm_stream_opencode.cpp
        src/llm_stream_agy.cpp
        src/llm_stream_codex.cpp
        src/event_schema.cpp
        src/common_posix.cpp
        src/platform/platform_posix.cpp
    )
    target_compile_definitions(hancho-cli PRIVATE ${PC_VERSION_DEFINITIONS})
    target_include_directories(hancho-cli PRIVATE src include)
    target_compile_options(hancho-cli PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(hancho-cli PRIVATE Threads::Threads util)

    # POSIX portability smoke test: shell quoting, file helpers, instance lock,
    # IPC transport and a real task queue run with /bin/sh.
    add_executable(posix_core_smoke
        tests/posix_core_smoke.cpp
        ${HANCHO_CORE_SOURCES}
    )
    target_compile_definitions(posix_core_smoke PRIVATE ${PC_VERSION_DEFINITIONS})
    target_include_directories(posix_core_smoke PRIVATE src include)
    target_compile_options(posix_core_smoke PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(posix_core_smoke PRIVATE Threads::Threads util)

    # Boundary-only smoke test shared with the Windows build.
    add_executable(platform_abstraction_smoke
        tests/platform_abstraction_smoke.cpp
        src/platform/platform_posix.cpp
    )
    target_include_directories(platform_abstraction_smoke PRIVATE src include)
    target_compile_options(platform_abstraction_smoke PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(platform_abstraction_smoke PRIVATE Threads::Threads util)

    # hancho-cli top TUI helpers and renderer (Cards 062-1, 062-2).
    add_executable(cli_top_smoke
        tests/cli_top_smoke.cpp
        src/common_posix.cpp
        src/ipc_client_posix.cpp
    )
    target_compile_definitions(cli_top_smoke PRIVATE ${PC_VERSION_DEFINITIONS})
    target_include_directories(cli_top_smoke PRIVATE src include tests)
    target_compile_options(cli_top_smoke PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(cli_top_smoke PRIVATE Threads::Threads util)

    # Browser UI skeleton smoke test (Card 061b)
    add_executable(webui_smoke
        tests/webui_smoke.cpp
        src/webui_server.cpp
        src/webui_assets.cpp
        src/remote_client.cpp
        src/remote_hosts.cpp
        src/common_posix.cpp
        src/platform/platform_posix.cpp
    )
    target_compile_definitions(webui_smoke PRIVATE ${PC_VERSION_DEFINITIONS})
    target_include_directories(webui_smoke PRIVATE src include tests)
    target_compile_options(webui_smoke PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(webui_smoke PRIVATE Threads::Threads util)
endif()
