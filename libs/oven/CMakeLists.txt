# Project setup
project (oven C)

add_subdirectory(pkgmgrs)
add_subdirectory(resolvers)
add_subdirectory(utils)
add_subdirectory(backends)

# initialize common sources
set(SRCS
    commands.c
    container.c
    oven.c
)

# add platform libraries
set(ADDITIONAL_LIBS)
if (UNIX)
    set(ADDITIONAL_LIBS ${ADDITIONAL_LIBS} -lpthread)
endif (UNIX)

add_library(oven STATIC ${SRCS})
target_include_directories(oven PUBLIC include)
target_link_libraries(oven PRIVATE chef-client oven-pkgmgrs oven-backend oven-builders oven-generators oven-resolvers platform)
target_link_libraries(oven PUBLIC vafs ${ADDITIONAL_LIBS} zstd-api libzstd_static)
