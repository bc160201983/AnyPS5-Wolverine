add_library(libAstroHybridHost SHARED EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/Export.cpp
)
target_include_directories(libAstroHybridHost PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(libAstroHybridHost PRIVATE libScePad)
configure_windows_unwind(libAstroHybridHost)
