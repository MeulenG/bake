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

#include <chef/diskbuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// the test writes straight into the partition streams
#include "../private.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  Assertion failed: %s\n", msg); \
            return 1; \
        } \
    } while(0)

#define IMAGE_PATH  "disk_test.img"
#define SECTOR_SIZE 512

// a region of the image that holds data, everything else must read back as zeros
struct region {
    unsigned long long offset;
    size_t             length;
    unsigned char      value;
};

static int __fill(struct chef_disk_partition* p, struct region* r, unsigned long long offset, size_t length, unsigned char value)
{
    unsigned char buffer[4096];

    if (length > sizeof(buffer)) {
        return -1;
    }
    memset(buffer, value, length);
    if (fseek(p->stream, (long)offset, SEEK_SET)) {
        return -1;
    }
    if (fwrite(buffer, 1, length, p->stream) != length) {
        return -1;
    }

    r->offset = p->sector_start * SECTOR_SIZE + offset;
    r->length = length;
    r->value = value;
    return 0;
}

static unsigned char __expected(struct region* regions, int count, unsigned long long offset)
{
    for (int i = 0; i < count; i++) {
        if (offset >= regions[i].offset && offset < regions[i].offset + regions[i].length) {
            return regions[i].value;
        }
    }
    return 0;
}

static void __cleanup(void)
{
    remove(IMAGE_PATH);
    remove("first-stream");
    remove("second-stream");
}

/**
 * Test: the image holds the partition data at the right offsets, zeros
 * everywhere else, has the full length and skips the zeros on disk
 */
static int test_image_sparse(void)
{
    struct chef_diskbuilder_params    params = { 0 };
    struct chef_disk_partition_params pparams = { 0 };
    struct chef_diskbuilder*          builder;
    struct chef_disk_partition*       first;
    struct chef_disk_partition*       second;
    struct region                     regions[3];
    unsigned long long                size;
    unsigned long long                offset;
    unsigned char*                    buffer;
    FILE*                             image;
    size_t                            read;

    __cleanup();

    params.schema = CHEF_DISK_SCHEMA_MBR;
    params.size = 64ULL * __MB;
    params.sector_size = SECTOR_SIZE;
    params.path = IMAGE_PATH;
    builder = chef_diskbuilder_new(&params);
    TEST_ASSERT(builder != NULL, "chef_diskbuilder_new should succeed");

    // the first partition ends in data
    pparams.name = "first";
    pparams.type = 0x0C;
    pparams.size = 64ULL * __KB;
    pparams.work_directory = ".";
    first = chef_diskbuilder_partition_new(builder, &pparams);
    TEST_ASSERT(first != NULL, "first partition should be created");
    TEST_ASSERT(__fill(first, &regions[0], 0, 512, 0xAA) == 0, "write to the first partition");
    TEST_ASSERT(__fill(first, &regions[1], 64ULL * __KB - 512, 512, 0xBB) == 0, "write to the first partition");

    // the second partition takes the rest of the disk and ends in zeros
    pparams.name = "second";
    pparams.size = 0;
    second = chef_diskbuilder_partition_new(builder, &pparams);
    TEST_ASSERT(second != NULL, "second partition should be created");
    TEST_ASSERT(__fill(second, &regions[2], 20ULL * __MB, 4096, 0xCC) == 0, "write to the second partition");

    TEST_ASSERT(chef_diskbuilder_partition_finish(first) == 0, "first partition should finish");
    TEST_ASSERT(chef_diskbuilder_partition_finish(second) == 0, "second partition should finish");
    TEST_ASSERT(chef_diskbuilder_finish(builder) == 0, "chef_diskbuilder_finish should succeed");

    // the image ends where the last partition ends
    size = (second->sector_start + second->sector_count) * SECTOR_SIZE;
    chef_diskbuilder_delete(builder);

    image = fopen(IMAGE_PATH, "rb");
    TEST_ASSERT(image != NULL, "image should exist");
    buffer = malloc(__MB);
    TEST_ASSERT(buffer != NULL, "allocate read buffer");

    // sector 0 is the MBR, check every byte after it
    offset = 0;
    while ((read = fread(buffer, 1, __MB, image)) > 0) {
        for (size_t i = 0; i < read; i++, offset++) {
            if (offset >= SECTOR_SIZE && buffer[i] != __expected(regions, 3, offset)) {
                printf("  byte at offset %llu is 0x%02X, expected 0x%02X\n",
                    offset, buffer[i], __expected(regions, 3, offset));
                fclose(image);
                free(buffer);
                return 1;
            }
        }
    }
    fclose(image);
    free(buffer);
    TEST_ASSERT(offset == size, "image should be as long as its partitions");

#ifndef _WIN32
    {
        struct stat st;
        TEST_ASSERT(stat(IMAGE_PATH, &st) == 0, "stat the image");
        printf("  image is %llu bytes, %llu bytes allocated\n",
            size, (unsigned long long)st.st_blocks * 512);
        TEST_ASSERT((unsigned long long)st.st_blocks * 512 < size / 8,
            "the zeros should not take up disk space");
    }
#endif

    __cleanup();
    return 0;
}

int main(void)
{
    printf("Running: Image: sparse\n");
    if (test_image_sparse()) {
        printf("  FAILED\n");
        return 1;
    }
    printf("  PASSED\n");
    return 0;
}
