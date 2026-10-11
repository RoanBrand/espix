# The doomgeneric engine, built from the fetched (not vendored) source.
#
# The backends for systems espix does not have are filtered here rather than in
# the fetch, so the checkout stays upstream and the filter is visible.
set(DG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/doomgeneric/doomgeneric")

file(GLOB DG_SRCS "${DG_DIR}/*.c")
list(FILTER DG_SRCS EXCLUDE REGEX "/doomgeneric_(allegro|emscripten|linuxvt|sdl|soso|sosox|win|xlib)\\.c$")
list(FILTER DG_SRCS EXCLUDE REGEX "/i_(allegromusic|allegrosound|sdlsound|sdlmusic)\\.c$")

# compat/ holds one empty SDL_mixer.h. FEATURE_SOUND makes i_sound.c include it
# and use nothing from it -- the mixing it would have asked SDL_mixer for is the
# platform module's job here -- so satisfying the include is cheaper than
# patching the engine, and third_party/ stays exactly as fetched.
idf_component_register(SRCS ${DG_SRCS}
                       INCLUDE_DIRS "${DG_DIR}" "compat")

# 320x200, and CMAP256 for an 8-bit framebuffer: the engine hands us palette
# indices and the platform converts, which is less memory than the 32-bit path
# and the same shape as the Quake 2 port next door.
# PUBLIC, not PRIVATE: the resolution and the pixel width are the engine's
# ABI. The platform layer includes doomgeneric.h and must agree on pixel_t, and
# it reads colors[] only when CMAP256 selected the 8-bit path.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    DOOMGENERIC_RESX=320
    DOOMGENERIC_RESY=200
    CMAP256
    # Sound effects, mixed by the platform module (doom_sound.c) over espix's
    # audio stream API. This is the switch that makes i_sound.c want both
    # DG_sound_module and DG_music_module -- so music exists as a stub, and
    # doom_espix.c keeps snd_musicdevice = SNDDEVICE_NONE until it is real.
    FEATURE_SOUND)

# Doom is 1993 C: it compiles with warnings on, so they are turned off here
# rather than patched, keeping the checkout exactly upstream.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-int-conversion
    -Wno-implicit-function-declaration
    -Wno-discarded-qualifiers
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-maybe-uninitialized
    -Wno-format
    -Wno-misleading-indentation
    -Wno-old-style-declaration
    -Wno-incompatible-pointer-types
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-unterminated-string-initialization
    -Wno-missing-field-initializers
    -Wno-char-subscripts
    -Wno-implicit-fallthrough
    # Doom predates -Werror, and this build turns warnings up. Errors stay
    # errors for our own code; the engine is upstream's to fix, not ours.
    -Wno-error)
