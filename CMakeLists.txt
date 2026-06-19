cmake_minimum_required(VERSION 3.16)
project(hft_framework LANGUAGES C CXX)

# ---- Default build type ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- LTO (Link-Time Optimization) ----
option(ENABLE_LTO "Enable Link-Time Optimization" OFF)
if(ENABLE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "LTO: ON")
endif()

# ---- PGO (Profile-Guided Optimization) ----
option(PGO_GENERATE "Build with PGO instrumentation (phase 1)" OFF)
option(PGO_USE "Build with PGO optimization (phase 2)" OFF)
set(PGO_DATA_DIR "${CMAKE_BINARY_DIR}/pgo_data" CACHE PATH "PGO profile data directory")

if(PGO_GENERATE AND PGO_USE)
    message(FATAL_ERROR "Cannot enable both PGO_GENERATE and PGO_USE simultaneously")
endif()

if(PGO_GENERATE)
    if(MSVC)
        add_compile_options(/GL)
        add_link_options(/LTCG /GENPROFILE)
    else()
        add_compile_options(-fprofile-generate=${PGO_DATA_DIR})
        add_link_options(-fprofile-generate=${PGO_DATA_DIR})
    endif()
    message(STATUS "PGO: GENERATE (MSVC: .pgd next to each binary)")
endif()

if(PGO_USE)
    if(MSVC)
        add_compile_options(/GL)
        add_link_options(/LTCG /USEPROFILE)
    else()
        add_compile_options(-fprofile-use=${PGO_DATA_DIR} -fprofile-correction)
        add_link_options(-fprofile-use=${PGO_DATA_DIR})
    endif()
    message(STATUS "PGO: USE (MSVC: .pgd next to each binary)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/dist")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/dist")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}/dist")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/lib")

# MSVC uses UTF-8 for project sources. C4828 is suppressed because the
# bundled CTP SDK headers are not UTF-8 encoded.
if(MSVC)
    add_compile_options(/utf-8 /wd4828)
endif()

option(ENABLE_PYTHON "Enable Python strategy support via pybind11" ON)

if(ENABLE_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
            OUTPUT_VARIABLE pybind11_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        find_package(pybind11 CONFIG REQUIRED)
    endif()
    message(STATUS "Python strategy support: ON (Python ${Python3_VERSION})")
endif()

set(CTP_SDK_DIR "" CACHE PATH "CTP SDK directory (required)")
if(NOT CTP_SDK_DIR)
    message(FATAL_ERROR "CTP_SDK_DIR is required. Download CTP SDK from http://www.simnow.com.cn/ "
                        "and pass -DCTP_SDK_DIR=<path> to cmake.\n"
                        "  Example: cmake -S . -B build -DCTP_SDK_DIR=C:/ctp_api/20250617_traderapi64_se_windows")
endif()
if(NOT EXISTS "${CTP_SDK_DIR}/ThostFtdcMdApi.h")
    message(FATAL_ERROR "ThostFtdcMdApi.h not found in CTP_SDK_DIR=${CTP_SDK_DIR}. "
                        "Please check the path points to the extracted CTP API directory.")
endif()

# QDP(上期所 FTD 低延迟通道):默认关闭,因为 SDK 不在仓库内,
# 需用户自行放置 + 通过 -DHFT_ENABLE_QDP=ON -DQDP_SDK_DIR=path 启用。
option(HFT_ENABLE_QDP "Enable QDP gateway (上期所 FTD 低延迟通道)" OFF)
set(QDP_SDK_DIR "${CMAKE_SOURCE_DIR}/qdp_api" CACHE PATH "QDP SDK directory (headers + libs)")

# 核心库源码：保留交易核心、策略和应用装配层，
# 控制台入口 main.cpp 排除在外。
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/test/")
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/main\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/api/hft_api\\.cpp$")
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/watchdog/")

if(NOT ENABLE_PYTHON)
    list(FILTER CORE_SOURCES EXCLUDE REGEX "py_strategy")
endif()

# HFT_ENABLE_QDP=OFF 时,glob 出来的 qdp_*.cpp 全部剔除,
# 这样仓库即使有 qdp 文件、用户不开关、也不需要 QDP SDK 就能编。
if(NOT HFT_ENABLE_QDP)
    list(FILTER CORE_SOURCES EXCLUDE REGEX "gateway/qdp_")
endif()

list(FILTER CORE_SOURCES EXCLUDE REGEX "metrics/")
option(ENABLE_METRICS "Enable Prometheus /metrics + REST API + WebUI" ON)
if(NOT ENABLE_METRICS)
    list(FILTER CORE_SOURCES EXCLUDE REGEX "webui/")
endif()

# hft_core：承载可复用的核心能力。
# SQLite 嵌入式数据库（FetchContent 管理，版本 3.49.1）
include(FetchContent)
FetchContent_Declare(
    sqlite3_amalgamation
    URL      https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
    URL_HASH SHA3_256=e7eb4cfb2d95626e782cfa748f534c74482f2c3c93f13ee828b9187ce05b2da7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3_amalgamation)

add_library(sqlite3 STATIC ${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_amalgamation_SOURCE_DIR})
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /w)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=2
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576
)

# cpp-httplib（header-only HTTP server for Prometheus /metrics endpoint）
# 优先用本地 third_party/httplib.h；不存在时 FetchContent 从 GitHub 下载。
if(ENABLE_METRICS)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/httplib.h")
        add_library(httplib_local INTERFACE)
        target_include_directories(httplib_local INTERFACE "${CMAKE_SOURCE_DIR}/third_party")
        add_library(httplib::httplib ALIAS httplib_local)
        message(STATUS "cpp-httplib: using local third_party/httplib.h")
    else()
        FetchContent_Declare(
            httplib
            URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.7.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(httplib)
        message(STATUS "cpp-httplib: downloaded via FetchContent")
    endif()
endif()

add_library(hft_core STATIC ${CORE_SOURCES})

target_include_directories(hft_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CTP_SDK_DIR}
)

target_link_libraries(hft_core PUBLIC sqlite3)

if(WIN32)
    target_link_libraries(hft_core PUBLIC ws2_32 winmm)
else()
    target_link_libraries(hft_core PUBLIC pthread uring)
endif()

option(HFT_HAS_DPDK "Enable DPDK kernel bypass (requires DPDK libraries)" OFF)
if(HFT_HAS_DPDK)
    target_compile_definitions(hft_core PUBLIC HFT_HAS_DPDK=1)
endif()

if(ENABLE_METRICS)
    target_link_libraries(hft_core PUBLIC httplib::httplib)
    target_compile_definitions(hft_core PUBLIC ENABLE_METRICS)
endif()

if(ENABLE_PYTHON)
    target_link_libraries(hft_core PUBLIC pybind11::embed)
    target_compile_definitions(hft_core PUBLIC ENABLE_PYTHON)
endif()

# ---- QDP SDK 接入(可选) ----
# 启用时,QDP 头文件目录加入 PUBLIC include,链接 lib,并向所有
# 链 hft_core 的下游传播 HFT_HAS_QDP=1。SDK 不在仓库,需要用户
# 自行放置到 ${QDP_SDK_DIR}。约定布局(参考 CTP):
#   ${QDP_SDK_DIR}/             # 头文件 + .lib/.so
#       FtdcMdApi.h FtdcTraderApi.h FtdcUserApiDataType.h FtdcUserApiStruct.h
#       qdpmduserapi.lib (Win) / qdpmduserapi.so (Linux)
#       qdptraderapi.lib / qdptraderapi.so
#       qdpmduserapi.dll / qdptraderapi.dll  # Windows runtime
if(HFT_ENABLE_QDP)
    if(NOT EXISTS "${QDP_SDK_DIR}")
        message(FATAL_ERROR "HFT_ENABLE_QDP=ON 但 QDP_SDK_DIR=${QDP_SDK_DIR} 不存在,请通过 -DQDP_SDK_DIR=path 指定")
    endif()
    target_include_directories(hft_core PUBLIC ${QDP_SDK_DIR})
    target_compile_definitions(hft_core PUBLIC HFT_HAS_QDP=1)
    if(WIN32)
        target_link_libraries(hft_core PUBLIC
            ${QDP_SDK_DIR}/qdpmduserapi.lib
            ${QDP_SDK_DIR}/qdptraderapi.lib
        )
    else()
        target_link_libraries(hft_core PUBLIC
            ${QDP_SDK_DIR}/qdpmduserapi.so
            ${QDP_SDK_DIR}/qdptraderapi.so
        )
    endif()
    message(STATUS "QDP gateway: ON (SDK=${QDP_SDK_DIR})")
else()
    message(STATUS "QDP gateway: OFF (HFT_ENABLE_QDP=OFF)")
endif()

# ---- C API 共享库 (FFI: Go, Rust, C#, Java JNI, Python ctypes) ----
option(ENABLE_CAPI "Build C API shared library (hft_capi.dll/.so)" ON)
if(ENABLE_CAPI)
    add_library(hft_capi SHARED src/api/hft_api.cpp)
    target_link_libraries(hft_capi PRIVATE hft_core)
    target_compile_definitions(hft_capi PRIVATE HFT_CAPI_EXPORTS)
    target_include_directories(hft_capi PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(WIN32)
        target_link_libraries(hft_capi PRIVATE
            ${CTP_SDK_DIR}/thostmduserapi_se.lib
            ${CTP_SDK_DIR}/thosttraderapi_se.lib
        )
    else()
        target_link_libraries(hft_capi PRIVATE
            ${CTP_SDK_DIR}/thostmduserapi_se.so
            ${CTP_SDK_DIR}/thosttraderapi_se.so
        )
    endif()
    set_target_properties(hft_capi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    )
endif()

# 控制台入口：当前仍保留原有 hft_framework 可执行程序，
# 只是将应用装配逻辑从 main.cpp 下沉到 hft_core 中。
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE hft_core)

if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
        VS_DEBUGGER_COMMAND_ARGUMENTS "--config \"${CMAKE_SOURCE_DIR}/dist/config.ini\""
        # Note: dist/config.ini is intentionally gitignored. Copy
        # config.example.ini to dist/config.ini and fill in credentials
        # before launching from the debugger.
    )
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${CTP_SDK_DIR}/thostmduserapi_se.lib
        ${CTP_SDK_DIR}/thosttraderapi_se.lib
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CTP_SDK_DIR}/thostmduserapi_se.dll"
            "${CTP_SDK_DIR}/thosttraderapi_se.dll"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config.example.ini"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/md_flow
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/td_flow
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/strategies
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/strategies"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/strategies
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${CTP_SDK_DIR}/thostmduserapi_se.so
        ${CTP_SDK_DIR}/thosttraderapi_se.so
        pthread
    )
    find_library(ICONV_LIB iconv)
    if(ICONV_LIB)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${ICONV_LIB})
    endif()

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CTP_SDK_DIR}/thostmduserapi_se.so"
            "${CTP_SDK_DIR}/thosttraderapi_se.so"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config.example.ini"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/md_flow
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/td_flow
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/strategies
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/strategies"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/strategies
    )
endif()

# ---- Watchdog standalone executable ----
option(ENABLE_WATCHDOG "Build standalone watchdog process" ON)
if(ENABLE_WATCHDOG)
    add_executable(hft_watchdog src/watchdog/watchdog_main.cpp)
    target_include_directories(hft_watchdog PRIVATE ${CMAKE_SOURCE_DIR}/src)
    set_target_properties(hft_watchdog PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    )
endif()

# ---- 测试目标 ----
option(ENABLE_TESTS "Build unit tests" ON)
if(ENABLE_TESTS)
    file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/test_*.cpp")
    list(FILTER TEST_SOURCES EXCLUDE REGEX "tests/test_main\\.cpp$")
    add_executable(hft_tests tests/test_main.cpp ${TEST_SOURCES})
    target_link_libraries(hft_tests PRIVATE hft_core)
    if(WIN32)
        target_link_libraries(hft_tests PRIVATE
            ${CTP_SDK_DIR}/thostmduserapi_se.lib
            ${CTP_SDK_DIR}/thosttraderapi_se.lib
        )
    endif()
    target_include_directories(hft_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    set_target_properties(hft_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    )
endif()

# ---- Latency benchmark 二进制 ----
# 独立 target,跟 hft_tests 平级。复用 hft_core 里的 QPCLatencyStats、
# OrderManager、PositionManager、ConditionalOrderManager 等。
option(ENABLE_BENCH "Build latency benchmark binary (hft_bench)" ON)
if(ENABLE_BENCH)
    file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS "bench/*.cpp")
    add_executable(hft_bench ${BENCH_SOURCES})
    target_link_libraries(hft_bench PRIVATE hft_core)
    if(WIN32)
        target_link_libraries(hft_bench PRIVATE
            ${CTP_SDK_DIR}/thostmduserapi_se.lib
            ${CTP_SDK_DIR}/thosttraderapi_se.lib
        )
    endif()
    target_include_directories(hft_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}
    )
    set_target_properties(hft_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    )
endif()
