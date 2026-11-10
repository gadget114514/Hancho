cmake_minimum_required(VERSION 3.20)
project(Hancho VERSION 0.9.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the MSVC runtime statically so the executables run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DECODE_ENABLE_SIXEL)

add_executable(Hancho
    src/main.cpp
    src/common.cpp
    src/process_manager.cpp
    src/taskqueue.cpp
    src/chain_guard.cpp
    src/ipc_server.cpp
    src/ipc_client.cpp
    src/cli.cpp
    src/mcp_server.cpp
    src/llm_stream.cpp
    src/llm_stream_claude.cpp
    src/llm_stream_opencode.cpp
    src/llm_stream_agy.cpp
    src/llm_stream_codex.cpp
    src/event_schema.cpp
    src/event_adapter.cpp
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

# Version macros for C++ (PC_VERSION) and the VERSIONINFO resource (src/version_info.rc2).
set(PC_VERSION_DEFINITIONS
    PC_VERSION="${PROJECT_VERSION}"
    PC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PC_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PC_VERSION_PATCH=${PROJECT_VERSION_PATCH}
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
    src/event_adapter.cpp
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
)

# Headless smoke test for the task queue engine (deps, retries, chain guard, persistence).
add_executable(taskqueue_smoke
    tests/taskqueue_smoke.cpp
    src/taskqueue.cpp
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
    src/taskqueue.cpp
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
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

# Headless smoke test for BT IPC actions (src/ipc_server.cpp + BtManager + TaskQueueManager + ProcessManager).
add_executable(bt_ipc_smoke
    tests/bt_ipc_smoke.cpp
    src/ipc_server.cpp
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
    src/taskqueue.cpp
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
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)




