# The app-facing espix headers live in the firmware tree and are the contract
# the loader resolves against, so the app includes them rather than copying
# them: one definition of the ABI, not two.
idf_component_register(SRCS "plasma.c"
                       INCLUDE_DIRS
                           "."
                           "../../../components/espix_proc/include"
                           "../../../components/espix_display/include")
