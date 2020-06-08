cmake_minimum_required(VERSION 3.5)
project(event VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)

##############################################
# Create target and set properties

add_library(event
    src/event.c
)

# Add an alias so that library can be used inside the build tree,
# e.g. when testing
add_library(Event::Event ALIAS event)

# Set target properties
target_include_directories(event
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(event
    PRIVATE $<$<C_COMPILER_ID:GNU>:-Wall -Werror>
)

if(TARGET Systemd::Libsystemd)
    target_compile_options(event PUBLIC -DEVENT_LIBSYSTEMD)
    target_link_libraries(event PUBLIC Systemd::Libsystemd)
endif()

##############################################
# Installation instructions

include(GNUInstallDirs)
set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/Event)

install(TARGETS event
    EXPORT event-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Ensure the exported target has the name Event and not event
# and if this is linked into a shared library, ensure it is PIC
set_target_properties(event
    PROPERTIES
        EXPORT_NAME Event
        POSITION_INDEPENDENT_CODE 1
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Export the targets to a script
install(EXPORT event-targets
    FILE
        EventTargets.cmake
    NAMESPACE
        Event::
    DESTINATION
        ${INSTALL_CONFIGDIR}
)

# Create a ConfigVersion.cmake file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/EventConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    ${CMAKE_CURRENT_LIST_DIR}/cmake/EventConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/EventConfig.cmake
    INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
)

# Install the config, configversion and custom find modules
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/EventConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/EventConfigVersion.cmake
    DESTINATION ${INSTALL_CONFIGDIR}
)

##############################################
# Exporting from the build tree
export(EXPORT event-targets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/EventTargets.cmake
    NAMESPACE Event::
)

# Register package in user's package registry
export(PACKAGE Event)
