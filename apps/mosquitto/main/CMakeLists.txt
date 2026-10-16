# The app's entry is upstream's src/mosquitto.c, not a file here: project_elf()
# compiles everything with -Dmain=app_main, so the broker's own main() *is*
# app_main. That means no main component of our own is needed, but the ELF
# loader always links one, so this is the empty one it gets.
idf_component_register(SRCS "main.c")
