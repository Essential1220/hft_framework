cmake_minimum_required(VERSION 3.16)
project(hft_framework LANGUAGES C CXX)

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

if(NOT ENABLE_PYTHON)
    list(FILTER CORE_SOURCES EXCLUDE REGEX "py_strategy")
endif()

# HFT_ENABLE_QDP=OFF 时,glob 出来的 qdp_*.cpp 全部剔除,
# 这样仓库即使有 qdp 文件、用户不开关、也不需要 QDP SDK 就能编。
if(NOT HFT_ENABLE_QDP)
    list(FILTER CORE_SOURCES EXCLUDE REGEX "gateway/qdp_")
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

add_library(hft_core STATIC ${CORE_SOURCES})

target_include_directories(hft_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CTP_SDK_DIR}
)

target_link_libraries(hft_core PUBLIC sqlite3)

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
        iconv  # GBK→UTF-8 转码，glibc 内置时无副作用，musl/Alpine 需安装 libiconv
    )

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
