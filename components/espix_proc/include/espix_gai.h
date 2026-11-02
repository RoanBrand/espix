/*
 * getaddrinfo()'s error text, which lwIP lacks: one table for the kernel's
 * gai_strerror (M-mode apps) and libespix's. The codes are lwIP's, and 205-209
 * espix's own for the ones lwIP has no number for.
 */
#pragma once

static inline const char *espix_gai_strerror(int err)
{
    switch (err) {
    case 0:   return "Success";
    case 200: return "Name or service not known";
    case 201: return "Servname not supported for ai_socktype";
    case 202: return "Non-recoverable failure in name resolution";
    case 203: return "Memory allocation failure";
    case 204: return "ai_family not supported";
    case 205: return "Temporary failure in name resolution";
    case 206: return "Bad value for ai_flags";
    case 207: return "ai_socktype not supported";
    case 208: return "Argument buffer overflow";
    case 209: return "System error";
    default:  return "Unknown error";
    }
}
