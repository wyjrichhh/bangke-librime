set(_glog_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})

find_path(Glog_INCLUDE_PATH glog/logging.h)

if (Glog_STATIC)
  if (WIN32)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib ${CMAKE_FIND_LIBRARY_SUFFIXES})
  else (WIN32)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
  endif (WIN32)
endif (Glog_STATIC)
find_library(Glog_LIBRARY NAMES glog glogd libglog)
if(Glog_INCLUDE_PATH AND Glog_LIBRARY)
  set(Glog_FOUND TRUE)
endif(Glog_INCLUDE_PATH AND Glog_LIBRARY)
if(Glog_FOUND)
  if(RIME_SHARED_GLOG)
    if(Glog_LIBRARY MATCHES "\\.a$")
      message(FATAL_ERROR
        "RIME_SHARED_GLOG requires a shared glog library; set Glog_LIBRARY "
        "and Glog_INCLUDE_PATH to the matching shared installation.")
    endif()
    include(CheckCXXSourceCompiles)
    function(_rime_check_shared_glog)
      # Check the header API without pulling in platform-specific link dependencies.
      set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
      set(CMAKE_REQUIRED_INCLUDES "${Glog_INCLUDE_PATH}")
      set(CMAKE_REQUIRED_DEFINITIONS
        -DGLOG_EXPORT= -DGLOG_NO_EXPORT= -DGLOG_DEPRECATED=
        -DGLOG_NO_ABBREVIATED_SEVERITIES)
      # A reused build may have selected a different glog installation.
      unset(RIME_GLOG_HAS_INIT_QUERY CACHE)
      check_cxx_source_compiles(
        "#include <glog/logging.h>
         int main() { return google::IsGoogleLoggingInitialized() ? 0 : 1; }"
        RIME_GLOG_HAS_INIT_QUERY)
      if(NOT RIME_GLOG_HAS_INIT_QUERY)
        message(FATAL_ERROR
          "RIME_SHARED_GLOG requires glog headers with "
          "google::IsGoogleLoggingInitialized(). Disable the option for legacy glog.")
      endif()
    endfunction()
    _rime_check_shared_glog()
  endif()
  if(NOT Glog_FIND_QUIETLY)
    message(STATUS "Found glog: ${Glog_LIBRARY}")
  endif(NOT Glog_FIND_QUIETLY)
else(Glog_FOUND)
  if(Glog_FIND_REQUIRED)
    message(FATAL_ERROR "Could not find glog library.")
  endif(Glog_FIND_REQUIRED)
endif(Glog_FOUND)

set(CMAKE_FIND_LIBRARY_SUFFIXES ${_glog_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES})
