# What an espix app includes to agree with the kernel about the ABI.
#
# An app is a standalone ESP-IDF project that the firmware loads at runtime, so
# nothing in the firmware's build reaches it: whatever the two have to agree on
# has to be opted into from here. There is one thing today -- the width of
# off_t, and therefore the layout of struct stat, which crosses the loader's
# boundary in both directions.
#
# The *name* is the point. An app author is saying "this is an espix app", not
# "I know off_t is 64 bits": the second is an implementation detail of espix's
# interface, and it is the kind of thing that grows. Anything of that kind
# belongs in this file rather than in each app, and an app should never have to
# learn about it. The reasoning for this particular one is in offt64.h.
#
# tools/check-abi.py checks that every app project includes this file, so
# forgetting is a failure in the firmware build -- which everyone runs -- rather
# than a struct stat written past the end of an app's buffer on a device.
#
# After project(), always: idf_build_set_property() is what IDF's own build
# system uses for this (tools/cmake/build.cmake), and it reaches every
# translation unit.

# Derived from this file's own location, so an app does not have to know it
# either. offt64.cmake is what needs it, to find offt64.h.
get_filename_component(ESPIX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include("${CMAKE_CURRENT_LIST_DIR}/offt64.cmake")
