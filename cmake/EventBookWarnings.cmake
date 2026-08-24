# eventbook::warnings is an INTERFACE target carrying the project's diagnostic
# policy. It is linked only by first-party targets, so third-party headers pulled
# in through vcpkg never trip -Werror.
#
# -Wconversion / -Wsign-conversion are deliberate: this project represents money
# and quantity as fixed-point integers, and a silent narrowing conversion is a
# correctness bug in market data, not a style nit.

add_library(eventbook_warnings INTERFACE)
add_library(eventbook::warnings ALIAS eventbook_warnings)

option(EVENTBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in project code" OFF)

if(MSVC)
  target_compile_options(eventbook_warnings INTERFACE /W4 /permissive-)
  if(EVENTBOOK_WARNINGS_AS_ERRORS)
    target_compile_options(eventbook_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(eventbook_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wformat=2
  )
  if(EVENTBOOK_WARNINGS_AS_ERRORS)
    target_compile_options(eventbook_warnings INTERFACE -Werror)
  endif()
endif()
