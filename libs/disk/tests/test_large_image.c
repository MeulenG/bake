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

// the test reads the partition stream directly
#include "../private.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  Assertion failed: %s\n", msg); \
            return 1; \
        } \
    } while(0)

#define IMAGE_PATH     "disk_large_test.img"
#define STREAM_PATH    "large-stream"
#define GPT_IMAGE_PATH "disk_gpt_test.img"
#define SECTOR_SIZE 512
#define __GB        (1024ULL * __MB)

static long long __tell(FILE* stream)
{
#ifdef _WIN32
    return _ftelli64(stream);
#else
    return (long long)ftello(stream);
#endif
}

static int __write_raw(struct chef_disk_filesystem* fs, unsigned long long offset, unsigned char value)
{
    unsigned char buffer[SECTOR_SIZE];

    memset(buffer, value, sizeof(buffer));
    return fs->write_raw(fs, &(struct chef_disk_fs_write_raw_params) {
        .buffer = buffer,
        .size = sizeof(buffer),
        .sector = offset / SECTOR_SIZE
    });
}

static int __check(FILE* stream, unsigned long long offset, unsigned char value)
{
    unsigned char buffer[SECTOR_SIZE];

    if (__disk_fseek(stream, (long long)offset, SEEK_SET)) {
        printf("  failed to seek to offset %llu\n", offset);
        return -1;
    }
    if (fread(buffer, 1, sizeof(buffer), stream) != sizeof(buffer)) {
        printf("  failed to read at offset %llu\n", offset);
        return -1;
    }
    for (size_t i = 0; i < sizeof(buffer); i++) {
        if (buffer[i] != value) {
            printf("  byte at offset %llu is 0x%02X, expected 0x%02X\n",
                offset + i, buffer[i], value);
            return -1;
        }
    }
    return 0;
}

/**
 * Test: a partition larger than 4GB gets its full size, and raw writes through
 * the FAT and MFS layers land at offsets that do not fit in 32 bits
 */
static int test_large_partition(void)
{
    struct chef_diskbuilder_params     params = { 0 };
    struct chef_disk_partition_params  pparams = { 0 };
    struct chef_disk_filesystem_params fsparams = { 0 };
    struct chef_diskbuilder*           builder;
    struct chef_disk_partition*        p;
    struct chef_disk_filesystem*       fat;
    struct chef_disk_filesystem*       mfs;
    unsigned long long                 size;

    remove(IMAGE_PATH);
    remove(STREAM_PATH);

    params.schema = CHEF_DISK_SCHEMA_MBR;
    params.size = 4160ULL * __MB;
    params.sector_size = SECTOR_SIZE;
    params.path = IMAGE_PATH;
    builder = chef_diskbuilder_new(&params);
    TEST_ASSERT(builder != NULL, "chef_diskbuilder_new should succeed");

    // takes the whole disk
    pparams.name = "large";
    pparams.guid = GPT_GUID_VALI_USERDATA;
    pparams.type = 0x0C;
    pparams.work_directory = ".";
    p = chef_diskbuilder_partition_new(builder, &pparams);
    TEST_ASSERT(p != NULL, "partition should be created");

    size = p->sector_count * SECTOR_SIZE;
    TEST_ASSERT(size > 4 * __GB, "partition should be larger than 4GB");
    TEST_ASSERT(__disk_fseek(p->stream, 0, SEEK_END) == 0, "seek to the end of the partition");
    TEST_ASSERT((unsigned long long)__tell(p->stream) == size, "partition stream should have the full size");

    fsparams.sector_size = SECTOR_SIZE;
    fat = chef_filesystem_fat32_new(p, &fsparams);
    TEST_ASSERT(fat != NULL, "FAT filesystem should be created");
    TEST_ASSERT(__write_raw(fat, 2 * __GB + __MB, 0xA1) == 0, "FAT write past 2GB");
    TEST_ASSERT(__write_raw(fat, 4 * __GB + __MB, 0xA2) == 0, "FAT write past 4GB");

    mfs = chef_filesystem_mfs_new(p, &fsparams);
    TEST_ASSERT(mfs != NULL, "MFS filesystem should be created");
    TEST_ASSERT(__write_raw(mfs, 3 * __GB, 0xB1) == 0, "MFS write past 2GB");
    TEST_ASSERT(__write_raw(mfs, 4 * __GB + 16 * __MB, 0xB2) == 0, "MFS write past 4GB");

    TEST_ASSERT(__check(p->stream, 2 * __GB + __MB, 0xA1) == 0, "FAT write past 2GB should land at its offset");
    TEST_ASSERT(__check(p->stream, 4 * __GB + __MB, 0xA2) == 0, "FAT write past 4GB should land at its offset");
    TEST_ASSERT(__check(p->stream, 3 * __GB, 0xB1) == 0, "MFS write past 2GB should land at its offset");
    TEST_ASSERT(__check(p->stream, 4 * __GB + 16 * __MB, 0xB2) == 0, "MFS write past 4GB should land at its offset");
    TEST_ASSERT(__check(p->stream, __MB, 0x00) == 0, "no write should wrap around to 1MB");

    fat->finish(fat);
    mfs->finish(mfs);

    // Finishing the image would copy the whole partition into it, which takes
    // a while, so the test stops at the partition. The image stays empty.
    fclose(p->stream);
    p->stream = NULL;
    chef_diskbuilder_delete(builder);
    remove(STREAM_PATH);
    remove(IMAGE_PATH);
    return 0;
}

static int __check_signature(FILE* stream, unsigned long long offset)
{
    char signature[8];

    if (__disk_fseek(stream, (long long)offset, SEEK_SET)) {
        return -1;
    }
    if (fread(signature, 1, sizeof(signature), stream) != sizeof(signature)) {
        return -1;
    }
    return memcmp(signature, "EFI PART", sizeof(signature)) == 0 ? 0 : -1;
}

static void __cleanup_gpt(void)
{
    remove(GPT_IMAGE_PATH);
    remove("gpt-first-stream");
    remove("gpt-second-stream");
}

/**
 * Test: in a GPT image larger than 2GB, the second partition and the backup
 * GPT header are written past 2GB, at the right offsets
 */
static int test_gpt_past_2gb(void)
{
    struct chef_diskbuilder_params    params = { 0 };
    struct chef_disk_partition_params pparams = { 0 };
    struct chef_diskbuilder*          builder;
    struct chef_disk_partition*       first;
    struct chef_disk_partition*       second;
    unsigned char                     buffer[SECTOR_SIZE];
    unsigned long long                start;
    unsigned long long                length;
    FILE*                             image;

    __cleanup_gpt();

    params.schema = CHEF_DISK_SCHEMA_GPT;
    params.size = 2112ULL * __MB;
    params.sector_size = SECTOR_SIZE;
    params.path = GPT_IMAGE_PATH;
    builder = chef_diskbuilder_new(&params);
    TEST_ASSERT(builder != NULL, "chef_diskbuilder_new should succeed");

    // the first partition is 2GB, so the second one starts past 2GB
    pparams.name = "gpt-first";
    pparams.guid = GPT_GUID_VALI_USERDATA;
    pparams.size = 2 * __GB;
    pparams.work_directory = ".";
    first = chef_diskbuilder_partition_new(builder, &pparams);
    TEST_ASSERT(first != NULL, "first partition should be created");

    pparams.name = "gpt-second";
    pparams.size = 0;
    second = chef_diskbuilder_partition_new(builder, &pparams);
    TEST_ASSERT(second != NULL, "second partition should be created");

    start = second->sector_start * SECTOR_SIZE;
    length = second->sector_count * SECTOR_SIZE;
    TEST_ASSERT(start > 2 * __GB, "second partition should start past 2GB");

    // mark the first and the last sector of the second partition
    memset(buffer, 0xC1, sizeof(buffer));
    TEST_ASSERT(__disk_fseek(second->stream, 0, SEEK_SET) == 0, "seek in the second partition");
    TEST_ASSERT(fwrite(buffer, 1, sizeof(buffer), second->stream) == sizeof(buffer), "write to the second partition");
    memset(buffer, 0xC2, sizeof(buffer));
    TEST_ASSERT(__disk_fseek(second->stream, (long long)(length - SECTOR_SIZE), SEEK_SET) == 0, "seek in the second partition");
    TEST_ASSERT(fwrite(buffer, 1, sizeof(buffer), second->stream) == sizeof(buffer), "write to the second partition");

    TEST_ASSERT(chef_diskbuilder_partition_finish(first) == 0, "first partition should finish");
    TEST_ASSERT(chef_diskbuilder_partition_finish(second) == 0, "second partition should finish");
    TEST_ASSERT(chef_diskbuilder_finish(builder) == 0, "chef_diskbuilder_finish should succeed");
    chef_diskbuilder_delete(builder);

    image = fopen(GPT_IMAGE_PATH, "rb");
    TEST_ASSERT(image != NULL, "image should exist");
    TEST_ASSERT(__disk_fseek(image, 0, SEEK_END) == 0, "seek to the end of the image");
    TEST_ASSERT((unsigned long long)__tell(image) == params.size, "image should have the full size");
    TEST_ASSERT(__check(image, start, 0xC1) == 0, "second partition should start at its offset");
    TEST_ASSERT(__check(image, start + length - SECTOR_SIZE, 0xC2) == 0, "second partition should end at its offset");
    TEST_ASSERT(__check_signature(image, SECTOR_SIZE) == 0, "primary GPT header should be in sector 1");
    TEST_ASSERT(__check_signature(image, params.size - SECTOR_SIZE) == 0, "backup GPT header should be in the last sector");
    fclose(image);

    __cleanup_gpt();
    return 0;
}

static const struct {
    const char* name;
    int (*func)(void);
} tests[] = {
    {"Image: partition larger than 4GB", test_large_partition},
    {"Image: GPT image past 2GB",        test_gpt_past_2gb},
};

int main(void)
{
    int failed = 0;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        printf("Running: %s\n", tests[i].name);
        if (tests[i].func()) {
            printf("  FAILED\n");
            failed++;
        } else {
            printf("  PASSED\n");
        }
    }
    return failed ? 1 : 0;
}
