# Copy the built PS4 updater-helper pkg into VitaPlex's pkg staging dir so it
# ships inside VitaPlex (as /app0/updater.pkg) and the user only installs one
# app.
#
# Run at BUILD time (cmake -P), not configure time, because the pkg does not
# exist yet when CMake configures. The file name is chosen by the pkg tool from
# the CONTENT_ID (IV0001-VPLX00003_00-...), not from the CMake target name, so
# it is located by glob rather than by a hard-coded path.

file(GLOB_RECURSE _pkgs "${SRC_DIR}/*VPLX00003*.pkg")
list(LENGTH _pkgs _n)
if(_n EQUAL 0)
    message(FATAL_ERROR "bundle_updater_pkg: no VPLX00003 pkg found under ${SRC_DIR}")
endif()
list(GET _pkgs 0 _pkg)
message(STATUS "bundle_updater_pkg: ${_pkg} -> ${DST}")
configure_file("${_pkg}" "${DST}" COPYONLY)
