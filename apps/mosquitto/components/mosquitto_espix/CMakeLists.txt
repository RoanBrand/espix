# The platform seam: the host facilities espix does not have yet (credentials,
# fork, umask), answered here so that not one line of mosquitto is patched.
idf_component_register(SRCS "espix_platform.c"
                       INCLUDE_DIRS "include")
