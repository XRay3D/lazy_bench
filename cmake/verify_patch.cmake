# Applies patches/cgal-once.patch to the pristine Lazy.h of the pinned CGAL
# release and checks that the result is exactly the headers in include/.
#
#   cmake -DPATCH_EXECUTABLE=... -DCGAL_ROOT=... -DSOURCE_DIR=... -DWORK_DIR=... -P verify_patch.cmake

file(REMOVE_RECURSE "${WORK_DIR}")
file(COPY "${CGAL_ROOT}/include/CGAL/Lazy.h" DESTINATION "${WORK_DIR}/include/CGAL")

# The patch uses the paths of the CGAL git tree (<Package>/include/CGAL/...);
# -p2 maps them onto the flat include/ of a release.
execute_process(
  COMMAND "${PATCH_EXECUTABLE}" -p2 -i "${SOURCE_DIR}/patches/cgal-once.patch"
  WORKING_DIRECTORY "${WORK_DIR}"
  RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "patches/cgal-once.patch does not apply to ${CGAL_ROOT}")
endif()

foreach(header CGAL/Lazy.h CGAL/STL_Extension/internal/once.h)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORK_DIR}/include/${header}" "${SOURCE_DIR}/include/${header}"
    RESULT_VARIABLE result)
  if(result)
    message(FATAL_ERROR "include/${header} differs from the pinned release plus patches/cgal-once.patch")
  endif()
endforeach()
