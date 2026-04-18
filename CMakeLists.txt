# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Default sdkconfig layering. dev.sh passes this explicitly too;
# the value here is just the fallback for bare `idf.py build`.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.pcm5102a")
endif()

# udp-music: drop the third-party udp_logger component. It heap-corrupts
# under load and its sources reference CONFIG_* symbols that don't exist
# when the component is disabled in sdkconfig, so partial-disable fails.
set(EXCLUDE_COMPONENTS udp_logger)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(snapclient)