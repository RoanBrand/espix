/*
 * An empty SDL_mixer.h, and it has to exist.
 *
 * With FEATURE_SOUND defined, i_sound.c does #include <SDL_mixer.h> -- and then
 * uses nothing from it, because the mixing it would have asked SDL_mixer for is
 * done by the platform module instead (doom_sound.c). So the include is a
 * formality the engine still insists on, and satisfying it with an empty header
 * is cheaper than patching the engine.
 *
 * It lives beside the engine's build rather than in third_party/, which stays
 * exactly as fetched.
 */
#pragma once
