/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#ifndef __PRIVATE_H__
#define __PRIVATE_H__

#include <chef/diskbuilder.h>
#include <chef/list.h>
#include <stdint.h>
#include <stdio.h>

#define __KB 1024
#define __MB (__KB * 1024)

#define __MIN(a, b) ((a) < (b) ? (a) : (b))

// fseek takes a long, which is 32 bits on Windows and cannot reach past 2GB,
// so seek with the 64-bit variants instead
static inline int __disk_fseek(FILE* stream, long long offset, int whence)
{
#ifdef _WIN32
    return _fseeki64(stream, offset, whence);
#else
    return fseeko(stream, (off_t)offset, whence);
#endif
}

struct chef_disk_partition {
    struct list_item               list_header;
    const char*                    name;
    const char*                    guid;
    uint8_t                        mbr_type;
    uint64_t                       sector_start;
    uint64_t                       sector_count;
    enum chef_partition_attributes attributes;
    FILE*                          stream;
};

#endif //!__PRIVATE_H__
