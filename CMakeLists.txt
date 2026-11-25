cmake_minimum_required(VERSION 3.20)
project(Hancho VERSION 0.10.0 LANGUAGES CXX)

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

option(HANCHO_BUILD_DEMO "Build demo mode and deterministic dummy LLM test targets (not shipped)" OFF)
if(HANCHO_BUILD_DEMO)
    add_compile_definitions(HANCHO_DEMO_BUILD)
endif()


if(WIN32)

# WCSP (WinConsoleServer protocol) client/server sources. Copied from
# WinConsoleServer; see src/wcsp/README.md.
set(WCSP_SOURCES
    src/wcsp/protocol.cpp
    src/wcsp/frame_reader.cpp
    src/wcsp/remote_session.cpp
)

set(HANCHO_SOURCES
    src/main.cpp
    src/common.cpp
    src/power.cpp
    src/platform/platform_windows.cpp
    src/process_manager.cpp
    src/taskqueue.cpp
    src/task_store.cpp
    src/direction_store.cpp
    src/routine.cpp
    src/triage.cpp
    src/triage_manager.cpp
    src/translation_store.cpp
    src/triage_text.cpp
    src/chain_guard.cpp
    src/ipc_server.cpp
    src/permit_draft.cpp
    src/ipc_server_remote.cpp
    src/webui_server.cpp
    src/ipmsg.cpp
    src/ipmsg_handler.cpp
    src/webui_assets.cpp
    src/ipc_client.cpp
    src/ephemeral_core.cpp
    src/remote_client.cpp
    src/remote_hosts.cpp
    src/cli.cpp
    src/card_markdown.cpp
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
    src/llm_config.cpp
    src/permit_config.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/bt_manager.cpp
    src/bt_manager_api.cpp
    src/gui_dashboard.cpp
    src/screenshot.cpp
    src/gui_ipmsg.cpp
    src/gui_theme.cpp
    src/term_launch.cpp
    src/term_window.cpp
    src/agent_template.rc
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
    src/Terminal/src/TerminalGridView.cpp
    src/term_server.cpp
    ${WCSP_SOURCES}
)

if(HANCHO_BUILD_DEMO)
    list(APPEND HANCHO_SOURCES src/demo.cpp)
endif()

# The program code is compiled once and linked into both the main Hancho binary
# and the Chat binary. Only the embedded app icon differs (HanchoChat.exe uses
# the chat icon for its file icon as well).
add_library(HanchoObjects OBJECT ${HANCHO_SOURCES})

target_compile_definitions(HanchoObjects PUBLIC ${PC_VERSION_DEFINITIONS})

target_include_directories(HanchoObjects PUBLIC
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
)

if(MSVC)
    target_compile_options(HanchoObjects PRIVATE /W4 /utf-8 /EHsc)
endif()

function(add_hancho_executable target rc_file)
    add_executable(${target} ${rc_file} $<TARGET_OBJECTS:HanchoObjects>)

    # Build as a GUI-subsystem binary so Windows never creates a console window
    # for it (shortcut, Run key, WMI auto-start or an MCP client all used to
    # flash one). Console output still works when a parent console or a
    # redirected pipe is inherited; see AttachParentConsoleForCli() in main.cpp.
    set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)

    target_compile_definitions(${target} PRIVATE ${PC_VERSION_DEFINITIONS})

    target_include_directories(${target} PRIVATE
        src
        include
        src/Terminal/include
        src/Terminal/src
        src/wcsp
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /EHsc)
        # The GUI subsystem expects WinMain by default; this program uses main().
        target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup
            "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
    endif()

    target_link_libraries(${target} PRIVATE
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
        iphlpapi
        powrprof
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
    )
endfunction()

add_hancho_executable(Hancho src/app.rc)
add_hancho_executable(HanchoChat src/app_chat.rc)
# The chat binary launches the core/gui from the same folder; building it also
# ensures Hancho.exe and the ConPTY support files are present in the output.
add_dependencies(HanchoChat Hancho)

add_custom_command(TARGET Hancho POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:Hancho>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:Hancho>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe to output directory"
)

add_custom_target(HanchoCore
    DEPENDS Hancho
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Hancho>"
        "$<TARGET_FILE_DIR:Hancho>/HanchoCore.exe"
    COMMENT "Copying Hancho.exe to HanchoCore.exe"
)

# Console client (human-friendly by default, --json for machine-readable output)
add_executable(hancho-cli
    src/cli_main.cpp
    src/cli.cpp
    src/card_markdown.cpp
    src/triage_text.cpp
    src/ipc_client.cpp
    src/ephemeral_core.cpp
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
    src/worker_engines.cpp
    src/llm_config.cpp
    src/permit_config.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/event_schema.cpp
    src/common.cpp
    src/platform/platform_windows.cpp
    src/power.cpp
    src/hancho-cli.rc
    src/agent_template.rc
)

if(HANCHO_BUILD_DEMO)
    target_sources(hancho-cli PRIVATE src/demo.cpp)
endif()

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
    powrprof
)

if(HANCHO_BUILD_DEMO)
    if(WIN32)
        set(DEMO_PLATFORM_SOURCE src/platform/platform_windows.cpp)
    else()
        set(DEMO_PLATFORM_SOURCE src/platform/platform_posix.cpp)
    endif()

    add_executable(hancho-dummy
        src/dummy_llm.cpp
        src/demo.cpp
        src/common.cpp
        ${DEMO_PLATFORM_SOURCE}
    )
    target_include_directories(hancho-dummy PRIVATE src include)
    if(MSVC)
        target_compile_options(hancho-dummy PRIVATE /W4 /utf-8 /EHsc)
    endif()
    target_link_libraries(hancho-dummy PRIVATE
        user32
        shell32
        advapi32
        crypt32
        ws2_32
        powrprof
    )

    add_executable(demo_smoke
        tests/demo_smoke.cpp
        src/demo.cpp
        src/llm_stream.cpp
        src/llm_stream_claude.cpp
        src/llm_stream_opencode.cpp
        src/llm_stream_agy.cpp
        src/llm_stream_codex.cpp
        src/event_schema.cpp
        src/event_adapter.cpp
        src/common.cpp
        ${DEMO_PLATFORM_SOURCE}
    )
    target_include_directories(demo_smoke PRIVATE src include)
    if(MSVC)
        target_compile_options(demo_smoke PRIVATE /W4 /utf-8 /EHsc)
    endif()
    target_link_libraries(demo_smoke PRIVATE
        user32
        shell32
        advapi32
        crypt32
        ws2_32
        powrprof
    )
    add_dependencies(demo_smoke hancho-dummy)
endif()

# wct: standalone WCSP terminal client for the console. It relays the remote
# ConPTY to the current terminal unchanged (raw passthrough).
add_executable(wct
    src/wct_main.cpp
    src/wct_client.cpp
    src/common.cpp
    src/ipc_client.cpp
    src/ephemeral_core.cpp
    src/remote_client.cpp
    src/remote_hosts.cpp
    ${WCSP_SOURCES}
)

target_compile_definitions(wct PRIVATE ${PC_VERSION_DEFINITIONS})

target_include_directories(wct PRIVATE
    src
    include
    src/wcsp
)

if(MSVC)
    target_compile_options(wct PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(wct PRIVATE
    user32
    shell32
    advapi32
    crypt32
    ws2_32
    powrprof
)

# HanchoTerm: standalone GUI terminal client for the WCSP terminal server.
# This used to be `Hancho.exe terminal`; it is now its own executable so the
# terminal client does not have to be shipped inside Hancho.exe.
add_executable(HanchoTerm
    src/term_main.cpp
    src/term_client.cpp
    src/term_window.cpp
    src/gui_theme.cpp
    src/common.cpp
    src/ipc_client.cpp
    src/ephemeral_core.cpp
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
    src/hancho-term.rc
    ${WCSP_SOURCES}
)

set_target_properties(HanchoTerm PROPERTIES WIN32_EXECUTABLE TRUE)

target_compile_definitions(HanchoTerm PRIVATE ${PC_VERSION_DEFINITIONS} PC_TERM)

target_include_directories(HanchoTerm PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
)

if(MSVC)
    target_compile_options(HanchoTerm PRIVATE /W4 /utf-8 /EHsc)
    target_link_options(HanchoTerm PRIVATE /ENTRY:mainCRTStartup
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()

target_link_libraries(HanchoTerm PRIVATE
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
    iphlpapi
    powrprof
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET HanchoTerm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:HanchoTerm>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:HanchoTerm>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe to output directory"
)

# End-to-end smoke test for wct: runs wct inside a local ConPTY against an
# isolated Hancho core and checks the WCSP echo round-trip.
add_executable(wct_smoke
    tests/wct_smoke.cpp
    src/wcsp/protocol.cpp
    src/wcsp/frame_reader.cpp
)

target_include_directories(wct_smoke PRIVATE src src/wcsp)

if(MSVC)
    target_compile_options(wct_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(wct_smoke PRIVATE ws2_32)
add_dependencies(wct_smoke wct)

# Headless smoke test for the task queue engine (deps, retries, chain guard, persistence).
add_executable(taskqueue_smoke
    tests/taskqueue_smoke.cpp
    src/taskqueue.cpp
    src/translation_store.cpp
    src/task_store.cpp
    src/direction_store.cpp
    src/card_markdown.cpp
    src/worker_engines.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/llm_config.cpp
    src/permit_config.cpp
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
    ${WCSP_SOURCES}
)

target_include_directories(taskqueue_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
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
    ws2_32
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
    ${WCSP_SOURCES}
)

target_include_directories(event_pipe_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
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
    ws2_32
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
    src/llm_config.cpp
    src/permit_config.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
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

# Headless test for the pure triage card logic and LLM advice validation (src/triage.cpp).
add_executable(triage_smoke
    tests/triage_smoke.cpp
    src/triage.cpp
    src/triage_text.cpp
    src/worker_engines.cpp
    src/permit_config.cpp
    src/platform/platform_windows.cpp
    src/common.cpp
)

target_include_directories(triage_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(triage_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(triage_smoke PRIVATE
    user32
    shell32
    advapi32
    psapi
    crypt32
)

# Headless smoke test for the translation cache and the translate-llm batch worker (src/translation_store.cpp).
add_executable(translation_smoke
    tests/translation_smoke.cpp
    src/translation_store.cpp
    src/taskqueue.cpp
    src/task_store.cpp
    src/direction_store.cpp
    src/worker_engines.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/llm_config.cpp
    src/permit_config.cpp
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
    ${WCSP_SOURCES}
)

target_include_directories(translation_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
)

if(MSVC)
    target_compile_options(translation_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(translation_smoke PRIVATE
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
    ws2_32
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET translation_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:translation_smoke>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:translation_smoke>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe for translation_smoke"
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
    src/permit_config.cpp
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

# Regression tests for BT retry, escalation and LLM-boundary behavior (tests/bt_regression_smoke.cpp).
add_executable(bt_regression_smoke
    tests/bt_regression_smoke.cpp
    src/behavior_tree.cpp
    src/behavior_tree_parse.cpp
    src/behavior_tree_vars.cpp
    src/behavior_tree_cond.cpp
    src/behavior_tree_exec.cpp
    src/behavior_tree_escalate.cpp
    src/behavior_tree_state.cpp
    src/behavior_tree_runtime.cpp
    src/worker_engines.cpp
    src/permit_config.cpp
    src/platform/platform_windows.cpp
    src/bt_debug_view.cpp
    src/common.cpp
)

target_include_directories(bt_regression_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(bt_regression_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(bt_regression_smoke PRIVATE
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
    src/llm_config.cpp
    src/permit_config.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/platform/platform_windows.cpp
    src/taskqueue.cpp
    src/translation_store.cpp
    src/task_store.cpp
    src/direction_store.cpp
    src/triage.cpp
    src/triage_manager.cpp
    src/triage_text.cpp
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
    ${WCSP_SOURCES}
)

target_include_directories(bt_integration_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
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
    src/permit_draft.cpp
    src/ipc_server_remote.cpp
    src/power.cpp
    src/webui_server.cpp
    src/ipmsg.cpp
    src/ipmsg_handler.cpp
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
    src/llm_config.cpp
    src/permit_config.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/platform/platform_windows.cpp
    src/taskqueue.cpp
    src/task_store.cpp
    src/direction_store.cpp
    src/routine.cpp
    src/triage.cpp
    src/triage_manager.cpp
    src/translation_store.cpp
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
    src/term_server.cpp
    ${WCSP_SOURCES}
)

target_include_directories(bt_ipc_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
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
    powrprof
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

# Headless smoke test for routines: save/list/run twice/show/testReport summary/invalid BT.
add_executable(routine_smoke
    tests/routine_smoke.cpp
    src/routine.cpp
    src/ipc_server.cpp
    src/permit_draft.cpp
    src/ipc_server_remote.cpp
    src/power.cpp
    src/webui_server.cpp
    src/ipmsg.cpp
    src/ipmsg_handler.cpp
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
    src/llm_config.cpp
    src/permit_config.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/platform/platform_windows.cpp
    src/taskqueue.cpp
    src/task_store.cpp
    src/direction_store.cpp
    src/triage.cpp
    src/triage_manager.cpp
    src/translation_store.cpp
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
    src/term_server.cpp
    ${WCSP_SOURCES}
)

target_include_directories(routine_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
    src/wcsp
)

if(MSVC)
    target_compile_options(routine_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(routine_smoke PRIVATE
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
    powrprof
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET routine_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:routine_smoke>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:routine_smoke>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe for routine_smoke"
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
    src/ephemeral_core.cpp
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

# Smoke test for the disposable CI core (`run --ephemeral`, Card 105-2).
add_executable(ephemeral_smoke
    tests/ephemeral_smoke.cpp
    src/ephemeral_core.cpp
    src/ipc_client.cpp
    src/common.cpp
)

add_dependencies(ephemeral_smoke Hancho hancho-cli)

target_include_directories(ephemeral_smoke PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(ephemeral_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(ephemeral_smoke PRIVATE
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
    src/ephemeral_core.cpp
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

# Headless regression test for the dashboard freeze when the WebUI is switched
# off: config_set must answer while a WebSocket request is still running.
add_executable(webui_config_smoke
    tests/webui_config_smoke.cpp
    src/power.cpp
    src/ipc_server.cpp
    src/permit_draft.cpp
    src/ipc_server_remote.cpp
    src/webui_server.cpp
    src/ipmsg.cpp
    src/ipmsg_handler.cpp
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
    src/llm_config.cpp
    src/permit_config.cpp
    src/worker_probe.cpp
    src/engine_health.cpp
    src/platform/platform_windows.cpp
    src/taskqueue.cpp
    src/task_store.cpp
    src/direction_store.cpp
    src/routine.cpp
    src/triage.cpp
    src/triage_manager.cpp
    src/translation_store.cpp
    src/triage_text.cpp
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
    src/term_server.cpp
    ${WCSP_SOURCES}
)

target_include_directories(webui_config_smoke PRIVATE
    src
    include
    tests
    src/Terminal/include
    src/Terminal/src
    src/wcsp
)

if(MSVC)
    target_compile_options(webui_config_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(webui_config_smoke PRIVATE
    powrprof
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

# Functional test for the WCSP terminal server: attaches to a running ConPTY job
# through the core's IPC endpoint and verifies replay, input and detach.
add_executable(term_server_smoke
    tests/term_server_smoke.cpp
    src/common.cpp
    src/ipc_client.cpp
    src/ephemeral_core.cpp
    ${WCSP_SOURCES}
)

target_include_directories(term_server_smoke PRIVATE
    src
    include
    src/wcsp
    tests
)

if(MSVC)
    target_compile_options(term_server_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(term_server_smoke PRIVATE
    ws2_32
    crypt32
    advapi32
    shell32
    user32
)

add_executable(hancho_test
    tests/framework/runner_main.cpp
    tests/framework/process_runner.cpp
    tests/api_tests/bt_boundary_tests.cpp
    src/behavior_tree.cpp
    src/behavior_tree_parse.cpp
    src/behavior_tree_vars.cpp
    src/behavior_tree_cond.cpp
    src/behavior_tree_exec.cpp
    src/behavior_tree_escalate.cpp
    src/behavior_tree_state.cpp
    src/behavior_tree_runtime.cpp
    src/worker_engines.cpp
    src/permit_config.cpp
    src/platform/platform_windows.cpp
    src/bt_debug_view.cpp
    src/common.cpp
)

add_dependencies(hancho_test hancho-cli)

target_compile_definitions(hancho_test PRIVATE PC_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories(hancho_test PRIVATE
    src
    include
    tests
)

if(MSVC)
    target_compile_options(hancho_test PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(hancho_test PRIVATE
    user32
    shell32
    advapi32
    psapi
    crypt32
)


else()
    # POSIX headless core and CLI (doc/obsoleted/design-v3.md 2.4). There is no GUI here:
    # the only screen is `hancho-cli top`.
    find_package(Threads REQUIRED)

    set(HANCHO_CORE_SOURCES
        src/common_posix.cpp
        src/power.cpp
        src/platform/platform_posix.cpp
        src/process_manager_posix.cpp
        src/taskqueue.cpp
        src/task_store.cpp
        src/direction_store.cpp
        src/routine.cpp
        src/llm_config.cpp
        src/permit_config.cpp
        src/triage.cpp
        src/triage_manager.cpp
        src/translation_store.cpp
        src/chain_guard_posix.cpp
        src/ipc_server.cpp
    src/permit_draft.cpp
        src/ipc_server_posix.cpp
        src/ipc_server_remote.cpp
        src/webui_server.cpp
        src/ipmsg.cpp
        src/ipmsg_handler.cpp
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
        src/llm_config.cpp
        src/permit_config.cpp
        src/worker_probe.cpp
        src/engine_health.cpp
    src/engine_health.cpp
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
        src/card_markdown.cpp
        src/ephemeral_core.cpp
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
        src/worker_engines.cpp
        src/llm_config.cpp
        src/permit_config.cpp
        src/worker_probe.cpp
        src/engine_health.cpp
    src/engine_health.cpp
        src/event_schema.cpp
        src/common_posix.cpp
        src/power.cpp
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
