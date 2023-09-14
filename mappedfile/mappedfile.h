/* mappedfile.h -- provides a class for memory mapped files
 *
 * Copyright (C) 2008-2009 Benjamin Kramer
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 */

#ifndef MAPPEDFILE_H
#define MAPPEDFILE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h>

/*!
 * Maps the specified file into memory.
 * \param path the path of the file being mapped
 * \param length pointer which the mapped length is written to.
 * \return the pointer to the mapping. On failure NULL is returned.
 */
#if defined(_WIN32) && defined(_NIRACLIENT_UTF16_PATHS_AND_NAMES)
char *map_file(const wchar_t *path, size_t *length);
#else
char *map_file(const char *path, size_t *length);
#endif

/*! Releases memory allocated by map_file().
 * \param data the pointer returned by map_file().
 * \param length the length returned by map_file().
 */
void unmap_file(char *data, size_t length);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MAPPEDFILE_H */
