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

# ─── Build the Go shared library ────────────────────────────────────────

set(NDND_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ndnd)
set(NDND_GO_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libndndsim_go.a)
set(NDND_GO_HEADER  ${CMAKE_CURRENT_BINARY_DIR}/libndndsim_go.h)

# Custom command to build the Go simulation library as a C archive.
# This produces libndndsim_go.a and libndndsim_go.h.
add_custom_command(
    OUTPUT ${NDND_GO_ARCHIVE} ${NDND_GO_HEADER}
    COMMAND ${CMAKE_COMMAND} -E env
        CGO_ENABLED=1
        CC=${CMAKE_C_COMPILER}
        go build
            -buildmode=c-archive
            -o ${NDND_GO_ARCHIVE}
            ./sim/cmd/
    WORKING_DIRECTORY ${NDND_SRC_DIR}
    COMMENT "Building NDNd Go simulation library"
    DEPENDS
        ${NDND_SRC_DIR}/sim/clock.go
        ${NDND_SRC_DIR}/sim/timer.go
        ${NDND_SRC_DIR}/sim/face.go
        ${NDND_SRC_DIR}/sim/fw_face.go
        ${NDND_SRC_DIR}/sim/forwarder.go
        ${NDND_SRC_DIR}/sim/node.go
        ${NDND_SRC_DIR}/sim/runtime.go
        ${NDND_SRC_DIR}/sim/cgo_export.go
        ${NDND_SRC_DIR}/sim/dv.go
        ${NDND_SRC_DIR}/sim/ndndsim_sim.h
        ${NDND_SRC_DIR}/sim/util.go
        ${NDND_SRC_DIR}/sim/cmd/main.go
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
target_include_directories(${libndndSIM} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# The Go archive requires pthreads and other system libraries
find_package(Threads REQUIRED)
target_link_libraries(${libndndSIM} PUBLIC Threads::Threads)

# Go runtime needs -lm -ldl on Linux
if(UNIX AND NOT APPLE)
    target_link_libraries(${libndndSIM} PUBLIC m dl)
endif()

# Note: examples/ is automatically processed by build_lib via build_lib_scan_examples
