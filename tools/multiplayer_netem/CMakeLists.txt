if(NOT WIN32)
    return()
endif()

add_library(multiplayer-netem-core STATIC
        netem_core.cpp
        netem_relay.cpp)
target_include_directories(multiplayer-netem-core PUBLIC
        ${PROJECT_SOURCE_DIR})
target_link_libraries(multiplayer-netem-core PUBLIC ws2_32)

add_executable(multiplayer-netem
        main.cpp)
target_link_libraries(multiplayer-netem multiplayer-netem-core)
