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

# Grid DV routing example (needs point-to-point-layout)
build_lib_example(
    NAME ndndsim-grid-dv
    SOURCE_FILES ndndsim-grid-dv.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
        ${libpoint-to-point-layout}
)

# DV multipath diamond example
build_lib_example(
    NAME ndndsim-dv-multipath
    SOURCE_FILES ndndsim-dv-multipath.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
)

# DV convergence grid example (needs point-to-point-layout)
build_lib_example(
    NAME ndndsim-dv-convergence
    SOURCE_FILES ndndsim-dv-convergence.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
        ${libpoint-to-point-layout}
)

# Sprint topology with random link/prefix churn
build_lib_example(
    NAME ndndsim-sprint-churn
    SOURCE_FILES ndndsim-sprint-churn.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
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



# Atlas general-purpose scenario
build_lib_example(
    NAME ndndsim-atlas-scenario
    SOURCE_FILES ndndsim-atlas-scenario.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
)

# Atlas multi-hop DV routing changes scenario
build_lib_example(
    NAME ndndsim-atlas-multihop-scenario
    SOURCE_FILES ndndsim-atlas-multihop-scenario.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
)

# Atlas routing-only scenario (no app traffic)
build_lib_example(
    NAME ndndsim-atlas-routing-scenario
    SOURCE_FILES ndndsim-atlas-routing-scenario.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
)

# Atlas two-phase churn scenario
build_lib_example(
    NAME ndndsim-atlas-churn-scenario
    SOURCE_FILES ndndsim-atlas-churn-scenario.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
)

# Atlas core-edge prefix-scale scenario
build_lib_example(
    NAME ndndsim-atlas-prefix-scale-scenario
    SOURCE_FILES ndndsim-atlas-prefix-scale-scenario.cc
    LIBRARIES_TO_LINK
        ${libndndSIM}
        ${libcore}
        ${libnetwork}
        ${libinternet}
        ${libpoint-to-point}
)
