/*
 * The one definition libsmb2 needs that cannot be passed as a compile
 * definition on this platform: _U_, upstream's marker for an unused argument.
 *
 * Upstream's Makefile.am and CMakeLists.txt both pass it as a definition:
 *
 *     -D_U_=__attribute__((unused))
 *
 * and that does not survive here. ESP-IDF hands the compile command to a shell,
 * and an unquoted opening parenthesis is a shell metacharacter: the definition
 * arrives split into three arguments -- '-D_U_=__attribute__', '-Dunused' and a
 * stray '-D)' -- and the compile dies with "syntax error near unexpected
 * token". A force-include puts no parentheses on the command line at all, which
 * is why cmake/offt64.h is a force-include for the same kind of reason.
 *
 * components/libsmb2/CMakeLists.txt does the including. _U_ is used in about
 * twenty declarations in lib/, in both parameter and variable position, so it
 * has to be defined rather than dropped: without it a declaration such as
 *
 *     unsigned char b[16] _U_, y[16] _U_;
 *
 * loses its attribute and does not parse.
 */
#pragma once

#ifndef _U_
#define _U_ __attribute__((unused))
#endif
