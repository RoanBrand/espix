# What an espix app includes to agree with the kernel about the ABI.
#
# An app is a standalone ESP-IDF project that the firmware loads at runtime, so
# nothing in the firmware's build reaches it: whatever the two have to agree on
# has to be opted into from here. There are two things today: the width of off_t,
# which every offset handed to lseek() carries across the loader's boundary, and
# how errno is reached.
#
# The *name* is the point. An app author is saying "this is an espix app", not
# "I know off_t is 64 bits": the second is an implementation detail of espix's
# interface, and it is the kind of thing that grows. Anything of that kind
# belongs in this file rather than in each app, and an app should never have to
# learn about it. The reasoning for this particular one is in offt64.h.
#
# tools/check-abi.py checks that every app project includes this file, so
# forgetting is a failure in the firmware build -- which everyone runs -- rather
# than a 32-bit offset handed to a 64-bit lseek() on a device.
#
# After project(), always: idf_build_set_property() is what IDF's own build
# system uses for this (tools/cmake/build.cmake), and it reaches every
# translation unit.

# Derived from this file's own location, so an app does not have to know it
# either. offt64.cmake is what needs it, to find offt64.h.
get_filename_component(ESPIX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include("${CMAKE_CURRENT_LIST_DIR}/offt64.cmake")

# errno through a function, as glibc spells it (`(*__errno_location())`). picolibc
# makes errno a thread-local variable, reached at a fixed offset from the thread
# pointer that is only known once the *firmware* is linked -- so an app compiled
# against it would need the loader to patch that offset in, and elf_loader has no
# TLS relocations at all. picolibc's own switch for this turns every `errno` into
# a call that the firmware answers with the calling thread's (abi_libc.c).
idf_build_set_property(COMPILE_DEFINITIONS "__PICOLIBC_ERRNO_FUNCTION=__errno" APPEND)

# The app ABI is picolibc's: the firmware publishes picolibc's names, and an app
# compiled against another libc's headers reaches its streams and errno in ways
# nothing in the firmware answers.
if(NOT CONFIG_LIBC_PICOLIBC)
    message(FATAL_ERROR "espix apps are built against picolibc, IDF's default; "
                        "delete this app's sdkconfig so it is regenerated")
endif()
