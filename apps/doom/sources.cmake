# The engine's sources and its build settings. The engine is fetched, not
# vendored: fetch-doom.sh pulls it into third_party/.
set(DG_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/doomgeneric/doomgeneric")

# The backends for systems espix does not have are filtered here rather than in
# the fetch, so the checkout stays upstream and the filter is visible.
file(GLOB DG_SRCS "${DG_DIR}/*.c")
list(FILTER DG_SRCS EXCLUDE REGEX "/doomgeneric_(allegro|emscripten|linuxvt|sdl|soso|sosox|win|xlib)\\.c$")
list(FILTER DG_SRCS EXCLUDE REGEX "/i_(allegromusic|allegrosound|sdlsound|sdlmusic)\\.c$")

# compat/ holds one empty SDL_mixer.h. FEATURE_SOUND makes i_sound.c include it
# and use nothing from it -- the mixing it would have asked SDL_mixer for is the
# platform module's job here -- so satisfying the include is cheaper than
# patching the engine.
set(DG_INC "${DG_DIR}" "${CMAKE_CURRENT_LIST_DIR}/components/doomgeneric/compat")

# 320x200, and CMAP256 for an 8-bit framebuffer: the engine hands us palette
# indices and the platform converts. The resolution and the pixel width are the
# engine's ABI, so the platform layer is compiled with them too.
# FEATURE_SOUND: sound effects, mixed by the platform module (doom_sound.c) over
# espix's audio stream API. It makes i_sound.c want both DG_sound_module and
# DG_music_module, so music exists as a stub and doom_espix.c keeps
# snd_musicdevice = SNDDEVICE_NONE until it is real.
set(DG_DEFS DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200 CMAP256 FEATURE_SOUND)

# Doom is 1993 C: it compiles with warnings on, so they are turned off here
# rather than patched, keeping the checkout exactly upstream.
set(DG_OPTS
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
    -Wno-error)
