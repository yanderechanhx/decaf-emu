# Can't use -Werror=shadow because of excmd
set(CMAKE_CXX_FLAGS "-Wshadow ${CMAKE_CXX_FLAGS}")

include_directories(".")

file(GLOB_RECURSE SOURCE_FILES *.cpp)
file(GLOB_RECURSE HEADER_FILES *.h)

add_executable(decaf-cli ${SOURCE_FILES} ${HEADER_FILES})

target_link_libraries(decaf-cli
    libdecaf
    libcpu
    common)

target_link_libraries(decaf-cli
    z
    m
    ${ADDRLIB_LIBRARIES}
    ${ASMJIT_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${GLBINDING_LIBRARIES}
    ${IMGUI_LIBRARIES}
    ${PUGIXML_LIBRARIES}
    ${OPENGL_LIBRARIES})

install(TARGETS decaf-cli RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
