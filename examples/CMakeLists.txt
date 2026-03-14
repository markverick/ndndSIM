# ndndSIM examples
#
# Common point-to-point examples
set(ndndsim_p2p_examples
    ndndsim-simple
    ndndsim-link-failure
    ndndsim-tree
    ndndsim-custom-app
    ndndsim-zipf
    ndndsim-topo-reader
    ndndsim-tree-tracers
)

foreach(example ${ndndsim_p2p_examples})
    build_lib_example(
        NAME ${example}
        SOURCE_FILES ${example}.cc
        LIBRARIES_TO_LINK
            ${libndndSIM}
            ${libcore}
            ${libnetwork}
            ${libinternet}
            ${libpoint-to-point}
    )
endforeach()

# Grid example (needs point-to-point-layout)
build_lib_example(
    NAME ndndsim-grid
    SOURCE_FILES ndndsim-grid.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
        ${libpoint-to-point-layout}
)

# CSMA example
build_lib_example(
    NAME ndndsim-csma
    SOURCE_FILES ndndsim-csma.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libcsma}
)

# WiFi example
build_lib_example(
    NAME ndndsim-wifi
    SOURCE_FILES ndndsim-wifi.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libwifi}
        ${libmobility}
)
