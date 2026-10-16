/*
 * Intentionally empty.
 *
 * The broker's own main() is the app's entry point -- project_elf() compiles
 * with -Dmain=app_main -- so there is nothing to translate and nothing to
 * wrap. Everything that stands between the broker and espix lives beside the
 * broker in components/mosquitto_espix, where it is visible next to the code
 * that needs it.
 */
