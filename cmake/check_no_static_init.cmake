# Fail the build if an ELF has static constructors to run (.init_array or
# .ctors). VitaPlex's quick menu plugin is linked without a C runtime, so
# nothing would run them, and a global object that needs one stays all
# zeros: build 2023's white did, and turned the song's lines transparent.
#
# Run at build time (cmake -P), after the plugin is linked:
#   cmake -DOBJDUMP=<objdump> -DELF=<file> -P check_no_static_init.cmake

if(NOT OBJDUMP)
    message(WARNING "check_no_static_init: no objdump, ${ELF} not checked")
    return()
endif()
execute_process(COMMAND "${OBJDUMP}" -h "${ELF}"
                OUTPUT_VARIABLE _sections
                RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "check_no_static_init: ${OBJDUMP} -h ${ELF} failed (${_rc})")
endif()
if(_sections MATCHES "\\.(init_array|ctors)")
    message(FATAL_ERROR "check_no_static_init: ${ELF} has static constructors "
                        "(.init_array or .ctors), which nothing in it runs. Give no "
                        "global object a constructor; build one where it is used.")
endif()
