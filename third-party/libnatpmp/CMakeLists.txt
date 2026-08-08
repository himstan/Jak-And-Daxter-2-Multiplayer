cmake_minimum_required(VERSION 3.5)

file(STRINGS VERSION PVER)

project(natpmp
        LANGUAGES C
        VERSION ${PVER}
        DESCRIPTION "A portable and fully compliant implementation of the NAT-PMP protocol"
        HOMEPAGE_URL "https://github.com/miniupnp/libnatpmp"
)

set (NATPMP_SOURCES
        natpmp.c
        getgateway.c
)

if (WIN32)
    set (NATPMP_SOURCES ${NATPMP_SOURCES} wingettimeofday.c)
endif (WIN32)

option(NATPMP_BUILD_TOOLS "Build libnatpmp command-line tools" OFF)
option(NATPMP_NO_INSTALL "Disable libnatpmp installation rules" ON)

# Library itself
add_library(natpmp STATIC ${NATPMP_SOURCES})
target_include_directories(natpmp PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(natpmp PUBLIC ENABLE_STRNATPMPERR NATPMP_MAX_RETRIES=3)

if (WIN32)
	target_link_libraries(natpmp PUBLIC ws2_32 iphlpapi)
	target_compile_definitions(natpmp PUBLIC NATPMP_STATICLIB)
endif (WIN32)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Haiku")
	target_link_libraries(natpmp PUBLIC network)
endif ()

if (NATPMP_BUILD_TOOLS)
	add_executable(natpmpc natpmpc.c)
	target_link_libraries(natpmpc natpmp)

	add_executable(testgetgateway
		testgetgateway.c
		getgateway.c)
	target_link_libraries(testgetgateway natpmp)
endif ()

# natpmp.pc
include(GNUInstallDirs)
configure_file(natpmp.pc.in natpmp.pc @ONLY)

if (NOT NATPMP_NO_INSTALL)
	set(NATPMP_INSTALL_TARGETS natpmp)
	if (NATPMP_BUILD_TOOLS)
		list(APPEND NATPMP_INSTALL_TARGETS natpmpc)
	endif ()
	install(TARGETS ${NATPMP_INSTALL_TARGETS}
			RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
			LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
			ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

	install(FILES natpmp.h natpmp_declspec.h
			DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/natpmp.pc
			DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif ()
