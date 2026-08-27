cmake_minimum_required(VERSION 3.20)

project(zfs LANGUAGES CXX)

option(ZFS_BUILD_TESTS "Build the ZFS test executable" ON)
option(ZFS_BUILD_TOOLS "Build command-line tools" ON)
option(ZFS_BUILD_ENGINE "Build the single-worker UCI engine" ON)
option(ZFS_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(ZFS_NATIVE "Optimize binaries for the build host CPU (not portable)" OFF)

if(ZFS_NATIVE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-march=native)
    else()
        message(FATAL_ERROR
            "ZFS_NATIVE is currently supported only by GCC and Clang")
    endif()
endif()

add_library(zfs
    src/attacks.cpp
    src/game.cpp
    src/position.cpp
)

target_include_directories(zfs PUBLIC include)
target_compile_features(zfs PUBLIC cxx_std_20)
target_compile_options(zfs PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

if(ZFS_BUILD_ENGINE)
    find_package(Threads REQUIRED)
    add_library(zfs_search
        engine/evaluate.cpp
        engine/search.cpp
        engine/tt.cpp
    )
    target_include_directories(zfs_search PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(zfs_search PUBLIC zfs)
    target_compile_features(zfs_search PUBLIC cxx_std_20)
    target_compile_options(zfs_search PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )

    add_executable(zfs_engine engine/uci.cpp)
    target_link_libraries(zfs_engine PRIVATE zfs_search Threads::Threads)
    target_compile_options(zfs_engine PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endif()

if(ZFS_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(zfs PUBLIC -fsanitize=address,undefined
        -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(zfs PUBLIC -fsanitize=address,undefined
        -fno-sanitize-recover=undefined)
endif()

if(ZFS_BUILD_TOOLS OR ZFS_BUILD_TESTS)
    add_library(zfs_eval_support
        tools/eval/openings.cpp
        tools/eval/stats.cpp
    )
    target_include_directories(zfs_eval_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(zfs_eval_support PUBLIC zfs)
    target_compile_features(zfs_eval_support PUBLIC cxx_std_20)
    target_compile_options(zfs_eval_support PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endif()

if(ZFS_BUILD_TOOLS)
    add_executable(zfs_perft tools/perft.cpp)
    target_link_libraries(zfs_perft PRIVATE zfs)

    add_executable(zfs_openings tools/openings.cpp)
    target_link_libraries(zfs_openings PRIVATE zfs_eval_support)
    target_compile_options(zfs_openings PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )

    add_executable(zfs_stats tools/stats.cpp)
    target_link_libraries(zfs_stats PRIVATE zfs_eval_support)
    target_compile_options(zfs_stats PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )

    if(ZFS_BUILD_ENGINE)
        add_executable(zfs_bench tools/bench.cpp)
        target_link_libraries(zfs_bench PRIVATE zfs_search)
        target_compile_options(zfs_bench PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
        )
    endif()

    if(UNIX)
        add_executable(zfs_match tools/match.cpp)
        target_link_libraries(zfs_match PRIVATE zfs_eval_support)
        target_compile_options(zfs_match PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
        )
    endif()
endif()

if(ZFS_BUILD_TESTS)
    enable_testing()
    add_executable(zfs_tests tests/zfs_tests.cpp)
    target_link_libraries(zfs_tests PRIVATE zfs)
    target_include_directories(zfs_tests PRIVATE src)
    add_test(NAME zfs_tests COMMAND zfs_tests)

    add_executable(zfs_differential_tests
        tests/differential_tests.cpp
        tests/reference_position.cpp
    )
    target_link_libraries(zfs_differential_tests PRIVATE zfs)
    add_test(NAME zfs_differential_tests COMMAND zfs_differential_tests)

    add_executable(zfs_viewer_bridge_tests
        tests/viewer_bridge_tests.cpp
        viewer/wasm/bridge.cpp
    )
    target_include_directories(zfs_viewer_bridge_tests PRIVATE viewer/wasm)
    target_link_libraries(zfs_viewer_bridge_tests PRIVATE zfs)
    add_test(NAME zfs_viewer_bridge_tests COMMAND zfs_viewer_bridge_tests)

    find_program(ZFS_NODE_EXECUTABLE node REQUIRED)
    add_test(NAME zfs_viewer_generated_smoke
        COMMAND ${ZFS_NODE_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_generated_smoke.mjs)

    add_executable(zfs_game_tests tests/game_tests.cpp)
    target_link_libraries(zfs_game_tests PRIVATE zfs)
    add_test(NAME zfs_game_tests COMMAND zfs_game_tests)

    add_executable(zfs_evaluation_tests tests/evaluation_tests.cpp)
    target_link_libraries(zfs_evaluation_tests PRIVATE zfs_eval_support)
    target_compile_definitions(zfs_evaluation_tests PRIVATE
        ZFS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME zfs_evaluation_tests COMMAND zfs_evaluation_tests)

    if(UNIX)
        add_executable(zfs_nonblocking_io_tests tests/nonblocking_io_tests.cpp)
        target_include_directories(zfs_nonblocking_io_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(zfs_nonblocking_io_tests PRIVATE cxx_std_20)
        target_compile_options(zfs_nonblocking_io_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
        )
        add_test(NAME zfs_nonblocking_io_tests COMMAND zfs_nonblocking_io_tests)
        set_tests_properties(zfs_nonblocking_io_tests PROPERTIES TIMEOUT 5)
    endif()

    if(ZFS_BUILD_ENGINE)
        add_executable(zfs_search_tests tests/search_tests.cpp)
        target_link_libraries(zfs_search_tests PRIVATE zfs_search)
        add_test(NAME zfs_search_tests COMMAND zfs_search_tests)

        if(UNIX)
            add_test(NAME zfs_uci_smoke
                COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/uci_smoke.sh
                        $<TARGET_FILE:zfs_engine>)
            set_tests_properties(zfs_uci_smoke PROPERTIES TIMEOUT 30)

            if(ZFS_BUILD_TOOLS)
                add_test(NAME zfs_match_smoke
                    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/match_smoke.sh
                            $<TARGET_FILE:zfs_match> $<TARGET_FILE:zfs_engine>)
                set_tests_properties(zfs_match_smoke PROPERTIES TIMEOUT 30)
            endif()
        endif()

        add_test(NAME zfs_viewer_engine_service
            COMMAND ${ZFS_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_engine_service_tests.mjs
                    $<TARGET_FILE:zfs_engine>)
        set_tests_properties(zfs_viewer_engine_service PROPERTIES TIMEOUT 30)
    endif()
endif()
