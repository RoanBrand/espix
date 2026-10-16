# The platform seam: the few Unix host facilities the broker needs that espix
# does not have, answered here so that not one line of mosquitto is patched.
idf_component_register(SRCS "espix_platform.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lwip
                       PRIV_REQUIRES esp_timer)
