add_library(kitchen-linux STATIC
    kitchen_init.c
    kitchen_step_clean.c
    kitchen_step_common.c
    kitchen_step_make.c
    kitchen_step_pack.c
    kitchen.c
    user.c
)
target_include_directories(kitchen-linux PRIVATE ../include)
target_link_libraries(kitchen-linux PUBLIC fridge oven ingredient platform)
