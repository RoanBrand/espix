# The sources under test are compiled directly, not through the espix_proc
# component, which needs the chip.
idf_component_register(
    SRCS
        "test_main.c"
        "test_sigset.c"
        "test_threads.c"
        "../../../proc_threads.c"
    INCLUDE_DIRS
        "../../../priv_include"
    REQUIRES
        unity
)
