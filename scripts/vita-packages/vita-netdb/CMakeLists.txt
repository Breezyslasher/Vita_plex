cmake_minimum_required(VERSION 3.10)
project(vita_netdb C)

# Build static library
add_library(vita_netdb STATIC vita_netdb.c)

# Include directory
target_include_directories(vita_netdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Install library and header
install(TARGETS vita_netdb ARCHIVE DESTINATION lib)
install(FILES vita_netdb.h DESTINATION include)
