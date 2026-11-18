
# add filesystem libraries
add_subdirectory(filesystems/fat)
add_subdirectory(filesystems/mfs)

set(SRCS
    assets/mbr-gpt.c
    assets/mbr.c

    api_fat.c
    api_mfs.c
    diskbuilder.c
)

add_library(libdisk STATIC ${SRCS})
target_include_directories(libdisk PUBLIC include)
target_link_libraries(libdisk PUBLIC libdisk-fat libdisk-mfs platform vlog)

# Tests
option(DISK_BUILD_TESTS "Build disk builder tests" ON)
if(DISK_BUILD_TESTS)
    add_executable(disk_test tests/test_diskbuilder.c)
    target_link_libraries(disk_test PRIVATE libdisk)
    add_test(NAME disk_test COMMAND disk_test)
endif()
