# Self-contained MP3 decoder component (public domain, lieff/minimp3).
#
# minimp3.h is NOT committed here. Download it once before building:
#   https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h
# and place it at components/minimp3/include/minimp3.h
#
# (See README.md for the exact command.)
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/include/minimp3.h")
    message(FATAL_ERROR
        "components/minimp3/include/minimp3.h is missing.\n"
        "Download it first:\n"
        "  curl -L -o components/minimp3/include/minimp3.h https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h")
endif()

idf_component_register(
    SRCS "minimp3.c"
    INCLUDE_DIRS "include"
)
