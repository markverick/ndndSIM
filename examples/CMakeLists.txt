# ndndSIM examples
set(ndndsim_examples
    ndndsim-simple
)

foreach(example ${ndndsim_examples})
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
