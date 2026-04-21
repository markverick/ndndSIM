# ndndSIM - ns-3 NDNd Simulation Module
#
# This module provides NDN networking using the NDNd Go stack
# running under ns-3's discrete-event simulation.
#
# Build process:
# 1. Patch ns-3 PointToPointNetDevice to recognise NDN EtherType (if needed)
# 2. Build the NDNd Go simulation library as a C archive (.a)
# 3. Build the C++ ns-3 module linking against the Go archive
#

cmake_minimum_required(VERSION 3.16)

# ─── Patch PointToPointNetDevice for NDN EtherType ──────────────────────
#
# ns-3's P2P device has a hardcoded switch mapping Ethernet ↔ PPP protocol
# numbers, with NS_ASSERT on unknown values. NDN (EtherType 0x8624, PPP
# 0x0077) isn't included upstream, so it must be patched in. This runs at
# configure time and is idempotent — it won't modify a file already patched.
#
set(_P2P_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src/point-to-point/model/point-to-point-net-device.cc")
if(EXISTS "${_P2P_SRC}")
    file(READ "${_P2P_SRC}" _p2p_content)
    string(FIND "${_p2p_content}" "0x8624" _ndn_found)
    if(_ndn_found EQUAL -1)
        message(STATUS "ndndSIM: Patching PointToPointNetDevice for NDN EtherType (0x8624)")
        # Add NDN case to PppToEther (PPP 0x0077 → Ethernet 0x8624)
        string(REPLACE
            "case 0x0057:\n        return 0x86DD; // IPv6\n    default:"
            "case 0x0057:\n        return 0x86DD; // IPv6\n    case 0x0077:\n        return 0x8624; // NDN (Named Data Networking)\n    default:"
            _p2p_content "${_p2p_content}")
        # Add NDN case to EtherToPpp (Ethernet 0x8624 → PPP 0x0077)
        string(REPLACE
            "case 0x86DD:\n        return 0x0057; // IPv6\n    default:"
            "case 0x86DD:\n        return 0x0057; // IPv6\n    case 0x8624:\n        return 0x0077; // NDN (Named Data Networking)\n    default:"
            _p2p_content "${_p2p_content}")
        file(WRITE "${_P2P_SRC}" "${_p2p_content}")
        message(STATUS "ndndSIM: Patch applied successfully")
    else()
        message(STATUS "ndndSIM: PointToPointNetDevice already has NDN EtherType — no patch needed")
    endif()
endif()

# ─── Find Go toolchain ──────────────────────────────────────────────────
#
# go.mod requires Go 1.24+. The setup script may download a recent Go
# toolchain into GOPATH. Try to locate it; fall back to system Go.
#
set(_GOPATH "$ENV{GOPATH}")
if(NOT _GOPATH)
    set(_GOPATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../gopath")
endif()

file(GLOB _GO_TOOLCHAINS "${_GOPATH}/pkg/mod/golang.org/toolchain@v0.0.1-go1.*.linux-amd64/bin/go")
if(_GO_TOOLCHAINS)
    list(SORT _GO_TOOLCHAINS)
    list(GET _GO_TOOLCHAINS -1 _GO_BIN)
else()
    if(EXISTS "/usr/local/go/bin/go")
        set(_GO_BIN "/usr/local/go/bin/go")
    else()
        find_program(_GO_BIN go)
    endif()
endif()

if(NOT _GO_BIN)
    message(FATAL_ERROR "ndndSIM: Go compiler not found. Run setup.sh or install Go 1.24+")
endif()

execute_process(
    COMMAND ${_GO_BIN} version
    OUTPUT_VARIABLE _GO_VERSION_STR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "go([0-9]+\\.[0-9]+(\\.[0-9]+)?)" _GO_VER_MATCH "${_GO_VERSION_STR}")
set(_GO_VERSION "${CMAKE_MATCH_1}")
if(_GO_VERSION VERSION_LESS "1.24")
    message(FATAL_ERROR "ndndSIM: selected Go is too old (${_GO_VERSION_STR}). Need Go 1.24+")
endif()

message(STATUS "ndndSIM: using Go: ${_GO_BIN}")

# ─── Build the Go shared library ────────────────────────────────────────
#
# go/build.sh handles everything:
#   1. Checks out pristine upstream ndnd (named-data/ndnd@dv2) into a temp worktree
#   2. Runs the AST transformer + overlay to produce a patched copy
#   3. Builds the CGo archive and runs the Go sim test suite
#
# The archive is written to go/libndndsim.a (same location used by the
# transformer's own go work sync step), so cmake can reuse a pre-built
# artifact from a prior run of setup.sh.

set(NDNDSIM_GO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/go)
set(NDND_GO_ARCHIVE ${NDNDSIM_GO_DIR}/libndndsim.a)
set(NDND_GO_HEADER  ${NDNDSIM_GO_DIR}/libndndsim.h)

# Track transformer and overlay sources so cmake knows when to rebuild.
file(GLOB_RECURSE NDND_GO_SOURCES
    "${NDNDSIM_GO_DIR}/transform/*.go"
    "${NDNDSIM_GO_DIR}/overlay/*.go"
    "${NDNDSIM_GO_DIR}/ndndsim/*.go"
)

# Custom command: delegate entirely to build.sh.
# OUT_LIB directs the archive (and its sibling .h) to NDNDSIM_GO_DIR.
add_custom_command(
    OUTPUT ${NDND_GO_ARCHIVE} ${NDND_GO_HEADER}
    COMMAND ${CMAKE_COMMAND} -E env
        GO=${_GO_BIN}
        OUT_LIB=${NDND_GO_ARCHIVE}
        bash ${NDNDSIM_GO_DIR}/build.sh
    COMMENT "Building NDNd Go simulation library (via transformer)"
    DEPENDS ${NDND_GO_SOURCES}
    VERBATIM
)

add_custom_target(ndndsim_go_lib DEPENDS ${NDND_GO_ARCHIVE} ${NDND_GO_HEADER})

# Create an imported static library target for the Go archive
add_library(ndndsim_go_static STATIC IMPORTED GLOBAL)
set_target_properties(ndndsim_go_static PROPERTIES
    IMPORTED_LOCATION ${NDND_GO_ARCHIVE}
)
add_dependencies(ndndsim_go_static ndndsim_go_lib)

# ─── NS-3 Module Definition ─────────────────────────────────────────────

set(source_files
    model/ndndsim-go-bridge.cc
    model/ndndsim-stack.cc
    model/ndndsim-app.cc
    apps/ndndsim-consumer.cc
    apps/ndndsim-consumer-zipf.cc
    apps/ndndsim-producer.cc
    helper/ndndsim-stack-helper.cc
    helper/ndndsim-app-helper.cc
    helper/ndndsim-topology-reader.cc
    helper/ndndsim-rate-tracer.cc
    helper/ndndsim-traffic-tracer.cc
    helper/ndndsim-link-tracer.cc
)

set(header_files
    model/ndndsim-go-bridge.h
    model/ndndsim-stack.h
    model/ndndsim-app.h
    apps/ndndsim-consumer.h
    apps/ndndsim-consumer-zipf.h
    apps/ndndsim-producer.h
    helper/ndndsim-stack-helper.h
    helper/ndndsim-app-helper.h
    helper/ndndsim-topology-reader.h
    helper/ndndsim-rate-tracer.h
    helper/ndndsim-traffic-tracer.h
    helper/ndndsim-link-tracer.h
)

build_lib(
    LIBNAME ndndSIM
    SOURCE_FILES ${source_files}
    HEADER_FILES ${header_files}
    LIBRARIES_TO_LINK
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
        ${libpoint-to-point-layout}
        ${libmobility}
        ndndsim_go_static
    TEST_SOURCES
        test/ndndsim-test-suite.cc
)

# Add include path for the generated Go header
target_include_directories(${libndndSIM} PRIVATE ${NDNDSIM_GO_DIR})

# The Go archive requires pthreads and other system libraries
find_package(Threads REQUIRED)
target_link_libraries(${libndndSIM} PUBLIC Threads::Threads)

# Go runtime needs -lm -ldl on Linux
if(UNIX AND NOT APPLE)
    target_link_libraries(${libndndSIM} PUBLIC m dl)
endif()

# Note: examples/ is automatically processed by build_lib via build_lib_scan_examples
