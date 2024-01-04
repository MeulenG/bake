/**
 * Copyright 2022, Philip Meulengracht
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

#include <backend.h>
#include <errno.h>
#include <libingredient.h>
#include <liboven.h>
#include <chef/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utils.h>
#include "private.h"
#include <vlog.h>

// include dirent.h for directory operations
#if defined(_WIN32) || defined(_WIN64)
#include <dirent_win32.h>
#else
#include <dirent.h>
#endif

#define OVEN_ROOT ".oven"
#define OVEN_INSTALL_ROOT OVEN_ROOT CHEF_PATH_SEPARATOR_S "output"

struct oven_recipe_context {
    const char*    name;
    const char*    relative_path;
    const char*    toolchain;
    struct list*   ingredients;
    struct scratch scratch;
};

struct oven_variables {
    const char* target_platform;
    const char* target_arch;
    const char* cwd;
};

struct oven_context {
    const char* const*         process_environment;
    const char*                install_root;
    struct oven_variables      variables;
    struct oven_recipe_context recipe;
};

struct generate_backend {
    const char* name;
    int       (*generate)(struct oven_backend_data* data, union oven_backend_options* options);
};

struct build_backend {
    const char* name;
    int       (*build)(struct oven_backend_data* data, union oven_backend_options* options);
};

static struct generate_backend g_genbackends[] = {
    { "configure", configure_main    },
    { "cmake",     cmake_main        },
    { "meson",     meson_config_main }
};

static struct build_backend g_buildbackends[] = {
    { "make",  make_main },
    { "meson", meson_build_main }
};

static struct oven_context g_oven = { 0 };

const char* __get_install_path(void)
{
    return g_oven.install_root;
}

const char* __get_platform(void)
{
    return g_oven.variables.target_platform;
}

const char* __get_architecture(void)
{
    return g_oven.variables.target_arch;
}

static int __get_cwd(char** bufferOut)
{
    char*  cwd;
    int    status;

    cwd = malloc(4096);
    if (cwd == NULL) {
        return -1;
    }

    status = platform_getcwd(cwd, 4096);
    if (status) {
        // buffer was too small
        VLOG_ERROR("oven", "could not get current working directory, buffer too small?\n");
        free(cwd);
        return -1;
    }
    *bufferOut = cwd;
    return 0;
}

static int __create_path(const char* path)
{
    if (platform_mkdir(path)) {
        if (errno != EEXIST) {
            VLOG_ERROR("oven", "oven: failed to create %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int oven_initialize(struct oven_parameters* parameters)
{
    int   status;
    char* cwd;
    char* root;
    char* installRoot;
    char  tmp[128];
    VLOG_DEBUG("oven", "oven_initialize()\n");

    if (parameters == NULL) {
        errno = EINVAL;
        return -1;
    }

    // get the current working directory
    status = __get_cwd(&cwd);
    if (status) {
        return -1;
    }

    // get basename of recipe
    strbasename(parameters->recipe_name, tmp, sizeof(tmp));

    // initialize oven paths
    root        = strpathcombine(cwd, OVEN_ROOT);
    installRoot = strpathcombine(cwd, OVEN_INSTALL_ROOT);
    if (root == NULL || installRoot == NULL) {
        free(root);
        return -1;
    }

    // update oven variables
    g_oven.variables.target_platform = strdup(parameters->target_platform);
    g_oven.variables.target_arch     = strdup(parameters->target_architecture);
    g_oven.variables.cwd             = cwd;

    // update oven context
    g_oven.process_environment = parameters->envp;
    g_oven.install_root = installRoot;

    // no active recipe
    memset(&g_oven.recipe, 0, sizeof(struct oven_recipe_context));

    // create paths
    status = __create_path(root);
    free(root);
    if (status) {
        return status;
    }
    status = __create_path(installRoot);
    return status;
}

void oven_cleanup(void)
{
    // cleanup resources by recipe context
    oven_recipe_end();

    free((void*)g_oven.variables.target_platform);
    free((void*)g_oven.variables.target_arch);
    free((void*)g_oven.variables.cwd);

    memset(&g_oven, 0, sizeof(struct oven_context));
}

static int __recreate_dir(const char* path)
{
    int status;

    status = platform_rmdir(path);
    if (status) {
        if (errno != ENOENT) {
            VLOG_ERROR("oven", "oven: failed to remove directory: %s\n", strerror(errno));
            return -1;
        }
    }

    status = platform_mkdir(path);
    if (status) {
        VLOG_ERROR("oven", "oven: failed to create directory: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int oven_clean(void)
{
    // For now all this does is reset all
    return __recreate_dir(OVEN_ROOT);
}

int oven_recipe_start(struct oven_recipe_options* options)
{
    char*          checkpointPath;
    size_t         relativePathLength;
    int            status;
    VLOG_DEBUG("oven", "oven_recipe_start()\n");

    if (g_oven.recipe.name) {
        VLOG_ERROR("oven", "oven: recipe already started\n");
        errno = ENOSYS;
        return -1;
    }

    g_oven.recipe.name          = strdup(options->name);
    g_oven.recipe.relative_path = strdup(options->relative_path);
    g_oven.recipe.toolchain     = options->toolchain != NULL ? strdup(options->toolchain) : NULL;
    g_oven.recipe.ingredients   = options->ingredients;
    
    // generate build and install directories
    status = scratch_setup(&(struct scratch_options) {
        .name = options->name,
        .install_path = g_oven.install_root,
        .project_path = g_oven.variables.cwd,
        .ingredients = options->ingredients,
        .imports = options->imports,
        .confined = options->confined,
    }, &g_oven.recipe.scratch);
    if (status) {
        VLOG_ERROR("oven", "oven: failed to setup scratch directory: %s\n", strerror(errno));
        return status;
    }
    return 0;
}

void oven_recipe_end(void)
{
    VLOG_DEBUG("oven", "oven_recipe_end()\n");
    free((void*)g_oven.recipe.name);
    free((void*)g_oven.recipe.relative_path);
    free((void*)g_oven.recipe.toolchain);
    free(g_oven.recipe.scratch.host_build_path);
    free(g_oven.recipe.scratch.host_checkpoint_path);
    free(g_oven.recipe.scratch.host_install_path);
    free(g_oven.recipe.scratch.build_root);
    free(g_oven.recipe.scratch.install_root);
    memset(&g_oven.recipe, 0, sizeof(struct oven_recipe_context));
}

int oven_clear_recipe_checkpoint(const char* name)
{
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (g_oven.recipe.scratch.host_checkpoint_path == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return oven_checkpoint_remove(g_oven.recipe.scratch.host_checkpoint_path, name);
}

static const char* __get_variable(const char* name)
{
    // Cross-compilation target variables.
    if (strcmp(name, "CHEF_TARGET_PLATFORM") == 0) {
        printf("CHEF_TARGET_PLATFORM: %s\n", g_oven.variables.target_platform);
        return g_oven.variables.target_platform;
    }
    if (strcmp(name, "CHEF_TARGET_ARCHITECTURE") == 0) {
        printf("CHEF_TARGET_ARCHITECTURE: %s\n", g_oven.variables.target_arch);
        return g_oven.variables.target_arch;
    }

    // Cross-compilation host variables.
    if (strcmp(name, "CHEF_HOST_PLATFORM") == 0) {
        printf("CHEF_HOST_PLATFORM: " CHEF_PLATFORM_STR "\n");
        return CHEF_PLATFORM_STR;
    }
    if (strcmp(name, "CHEF_HOST_ARCHITECTURE") == 0) {
        printf("CHEF_HOST_ARCHITECTURE: " CHEF_ARCHITECTURE_STR "\n");
        return CHEF_ARCHITECTURE_STR;
    }

    if (strcmp(name, "TOOLCHAIN_PREFIX") == 0) {
        return g_oven.recipe.toolchain;
    }
    if (strcmp(name, "PROJECT_PATH") == 0) {
        if (!g_oven.recipe.scratch.confined) {
            return g_oven.variables.cwd;
        }
        return g_oven.recipe.scratch.project_root;
    }
    if (strcmp(name, "INSTALL_PREFIX") == 0) {
        if (!g_oven.recipe.scratch.confined) {
            return g_oven.install_root;
        }
        return g_oven.recipe.scratch.install_root;
    }
    return NULL;
}

static int __expand_variable(char** at, char** buffer, int* index, size_t* maxLength)
{
    const char* start = *at;
    char*       end   = strchr(start, ']');
    if (end && end[1] == ']') {
        char* variable;

        // fixup at
        *at = (end + 2);

        start += 3; // skip $[[

        // trim leading spaces
        while (*start == ' ') {
            start++;
        }

        // trim trailing spaces
        end--;
        while (*end == ' ') {
            end--;
        }
        end++;
        
        variable = strndup(start, end - start);
        if (variable != NULL) {
            const char* value = __get_variable(variable);
            free(variable);
            if (value != NULL) {
                size_t valueLength = strlen(value);
                if (valueLength > *maxLength) {
                    *maxLength = valueLength;
                    errno = ENOSPC;
                    return -1;
                }
                
                memcpy(&(*buffer)[*index], value, valueLength);
                *index += valueLength;
                return 0;
            } else {
                errno = ENOENT;
                return -1;
            }
        } else {
            errno = ENOMEM;
            return -1;
        }
    }
    errno = EINVAL;
    return -1;
}

static int __expand_environment_variable(char** at, char** buffer, int* index, size_t* maxLength)
{
    const char* start = *at;
    char*       end   = strchr(start, '}');
    if (end) {
        char* variable;
        
        // fixup at
        *at = end + 1;

        start += 2; // skip ${

        // trim leading spaces
        while (*start == ' ') {
            start++;
        }

        // trim trailing spaces
        end--;
        while (*end == ' ') {
            end--;
        }
        end++;

        variable = strndup(start, end - start);
        if (variable != NULL) {
            char* value = getenv(variable);
            free(variable);
            if (value != NULL) {
                size_t valueLength = strlen(value);
                if (valueLength > *maxLength) {
                    *maxLength = valueLength;
                    errno = ENOSPC;
                    return -1;
                }
                
                memcpy(&(*buffer)[*index], value, valueLength);
                *index += valueLength;
                return 0;
            }
        } else {
            errno = ENOENT;
            return -1;
        }
    } else {
        errno = ENOMEM;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

static void* __resize_buffer(void* buffer, size_t length)
{
    void* biggerBuffer = calloc(1, length);
    if (!biggerBuffer) {
        return NULL;
    }
    strcat(biggerBuffer, buffer);
    free(buffer);
    return biggerBuffer;
}

const char* oven_preprocess_text(const char* original)
{
    const char* itr = original;
    const char* result;
    char*       buffer;
    size_t      bufferSize = 4096;
    int         index;

    if (original == NULL) {
        return NULL;
    }
    
    buffer = calloc(1, bufferSize);
    if (buffer == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    // trim spaces
    while (*itr == ' ') {
        itr++;
    }
    
    index = 0;
    while (*itr) {
        if (strncmp(itr, "$[[", 3) == 0) {
            // handle variables
            size_t spaceLeft = bufferSize - index;
            int    status;
            do {
                status = __expand_variable((char**)&itr, &buffer, &index, &spaceLeft);
                if (status) {
                    if (errno == ENOSPC) {
                        buffer = __resize_buffer(buffer, bufferSize + spaceLeft + 1024);
                        if (!buffer) {
                            free(buffer);
                            return NULL;
                        }
                    } else {
                        break;
                    }
                }
            } while (status != 0);
        } else if (strncmp(itr, "$[", 2) == 0) {
            // handle environment variables
            size_t spaceLeft = bufferSize - index;
            int    status;
            do {
                status = __expand_environment_variable((char**)&itr, &buffer, &index, &spaceLeft);
                if (status) {
                    if (errno == ENOSPC) {
                        buffer = __resize_buffer(buffer, bufferSize + spaceLeft + 1024);
                        if (!buffer) {
                            free(buffer);
                            return NULL;
                        }
                    } else {
                        break;
                    }
                }
                
            } while (status != 0);
        } else {
            buffer[index++] = *itr;
            itr++;
        }
    }
    
    result = strdup(buffer);
    free(buffer);
    return result;
}

const char* __build_argument_string(struct list* argumentList)
{
    struct list_item* item;
    char*             argumentString;
    char*             argumentItr;
    size_t            totalLength = 0;

    // allocate memory for the string
    argumentString = (char*)malloc(4096);
    if (argumentString == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memset(argumentString, 0, 4096);

    // copy arguments into buffer
    argumentItr = argumentString;
    list_foreach(argumentList, item) {
        struct oven_value_item* value = (struct oven_value_item*)item;
        const char*             valueString = oven_preprocess_text(value->value);
        size_t                  valueLength = strlen(valueString);

        if (valueLength > 0 && (totalLength + valueLength + 2) < 4096) {
            strcpy(argumentItr, valueString);
            
            totalLength += valueLength;
            argumentItr += valueLength;
            if (item->next) {
                *argumentItr = ' ';
                argumentItr++;
            }
        }
        free((void*)valueString);
    }
    return argumentString;
}

static struct generate_backend* __get_generate_backend(const char* name)
{
    for (int i = 0; i < sizeof(g_genbackends) / sizeof(struct generate_backend); i++) {
        if (!strcmp(name, g_genbackends[i].name)) {
            return &g_genbackends[i];
        }
    }
    return NULL;
}

static struct chef_keypair_item* __preprocess_keypair(struct chef_keypair_item* original)
{
    struct chef_keypair_item* keypair;

    keypair = (struct chef_keypair_item*)malloc(sizeof(struct chef_keypair_item));
    if (!keypair) {
        return NULL;
    }

    keypair->key   = strdup(original->key);
    keypair->value = oven_preprocess_text(original->value);
    return keypair;
}

static struct list* __preprocess_keypair_list(struct list* original)
{
    struct list*      processed = malloc(sizeof(struct list));
    struct list_item* item;

    if (!processed) {
        VLOG_ERROR("oven", "oven: failed to allocate memory environment preprocessor\n");
        return original;
    }

    list_init(processed);
    list_foreach(original, item) {
        struct chef_keypair_item* keypair          = (struct chef_keypair_item*)item;
        struct chef_keypair_item* processedKeypair = __preprocess_keypair(keypair);
        if (!processedKeypair) {
            VLOG_ERROR("oven", "oven: failed to allocate memory environment preprocessor\n");
            break;
        }

        list_add(processed, &processedKeypair->list_header);
    }
    return processed;
}

static const char* __append_ingredients_system_path(const char* original)
{
    // '-isystem-after ' (15)
    // '/include' (8)
    // space between (1)
    // zero terminator (1)
    size_t length = strlen(original) + strlen("") + 26;
    char*  buffer = calloc(length, 1);
    if (buffer == NULL) {
        return NULL;    
    }
    
    snprintf(buffer, length - 1, "%s -isystem-after %s/include", original, "");
    return buffer;
}

static struct chef_keypair_item* __build_ingredients_system_path_keypair(const char* key)
{
    // '-isystem-after ' (15)
    // '/include' (8)
    // zero terminator (1)
    size_t length = strlen("") + 25;
    char*  buffer = calloc(length, 1);
    struct chef_keypair_item* item = calloc(sizeof(struct chef_keypair_item), 1);
    if (buffer == NULL || item == NULL) {
        free(buffer);
        return NULL;
    }
    item->key = strdup(key);
    if (item->key == NULL) {
        free(buffer);
        free(item);
    }

    snprintf(buffer, length - 1, "-isystem-after %s/include", "");
    item->value = buffer;
    return item;
}

static int __append_or_update_environ_flags(struct list* environment)
{
    // Look and update/add the following language flags to account for
    // ingredient include paths
    struct list_item* item;
    struct {
        const char* ident;
        int         fixed;
    } idents[] = {
        { "CFLAGS", 0 },
        { "CXXFLAGS", 0 },
        { NULL, 0 }
    };

    // Update any environmental variable already provided by recipe
    list_foreach(environment, item) {
        struct chef_keypair_item* keypair = (struct chef_keypair_item*)item;
        for (int i = 0; idents[i].ident != NULL; i++) {
            if (!strcmp(keypair->key, idents[i].ident)) {
                const char* tmp = keypair->value;
                keypair->value = __append_ingredients_system_path(tmp);
                if (keypair->value == NULL) {
                    keypair->value = tmp;
                    return -1;
                }
                free((void*)tmp);
                idents[i].fixed = 1;
            }
        }
    }

    // Add any that was not provided
    for (int i = 0; idents[i].ident != NULL; i++) {
        if (!idents[i].fixed) {
            item = (struct list_item*)__build_ingredients_system_path_keypair(idents[i].ident);
            if (item == NULL) {
                return -1;
            }
            list_add(environment, item);
        }
    }
    return 0;
}

static void __cleanup_environment(struct list* keypairs)
{
    struct list_item* item;

    if (keypairs == NULL) {
        return;
    }

    for (item = keypairs->head; item != NULL;) {
        struct list_item*         next    = item->next;
        struct chef_keypair_item* keypair = (struct chef_keypair_item*)item;

        free((void*)keypair->key);
        free((void*)keypair->value);
        free(keypair);

        item = next;
    }
    free(keypairs);
}

static void __cleanup_backend_data(struct oven_backend_data* data)
{
    __cleanup_environment(data->environment);
    free((void*)data->arguments);

    free((void*)data->paths.root);
    free((void*)data->paths.project);
}

static int __initialize_backend_data(struct oven_backend_data* data, const char* profile, struct list* arguments, struct list* environment)
{
    int                      status;
    char*                    path;

    // reset the datastructure
    memset(data, 0, sizeof(struct oven_backend_data));

    status = __get_cwd(&path);
    if (status) {
        return status;
    }
    data->paths.root = path;
    
    path = strpathcombine(data->paths.root, g_oven.recipe.relative_path);
    if (path == NULL) {
        free((void*)data->paths.root);
        return status;
    }
    data->paths.project = path;

    data->project_name        = g_oven.recipe.name;
    data->profile_name        = profile != NULL ? profile : "Release";
    data->process_environment = g_oven.process_environment;
    data->ingredients         = g_oven.recipe.ingredients;

    data->platform.host_platform = CHEF_PLATFORM_STR;
    data->platform.host_architecture = CHEF_ARCHITECTURE_STR;
    data->platform.target_platform = g_oven.variables.target_platform;
    data->platform.target_architecture = g_oven.variables.target_arch;
    
    data->paths.install = g_oven.recipe.scratch.install_root;
    data->paths.build   = g_oven.recipe.scratch.build_root;
    
    data->environment = __preprocess_keypair_list(environment);
    if (!data->environment) {
        __cleanup_backend_data(data);
        return -1;
    }

    //if (__append_or_update_environ_flags(data->environment)) {
    //    __cleanup_backend_data(data);
    //    return -1;
    //}

    data->arguments = __build_argument_string(arguments);
    if (!data->arguments) {
        __cleanup_backend_data(data);
        return -1;
    }
    return 0;
}

int oven_configure(struct oven_generate_options* options)
{
    struct generate_backend* backend;
    struct oven_backend_data data;
    int                      status;
    char*                    path;

    if (!options) {
        errno = EINVAL;
        return -1;
    }

    backend = __get_generate_backend(options->system);
    if (!backend) {
        errno = ENOSYS;
        return -1;
    }

    // check if we already have done this step
    if (oven_checkpoint_contains(g_oven.recipe.scratch.host_checkpoint_path, options->name)) {
        printf("nothing to be done for %s\n", options->name);
        return 0;
    }
    
    printf("running step %s\n", options->name);
    status = __initialize_backend_data(&data, options->profile, options->arguments, options->environment);
    if (status) {
        return status;
    }

    status = scratch_enter(&g_oven.recipe.scratch);
    if (status) {
        VLOG_ERROR("oven", "oven_configure: failed to enter scratch area: %s\n", strerror(errno));
        return status;
    }

    status = backend->generate(&data, options->system_options);
    if (status == 0) {
        status = oven_checkpoint_create(g_oven.recipe.scratch.host_checkpoint_path, options->name);
    }

    status = scratch_leave(&g_oven.recipe.scratch);
    if (status) {
        VLOG_ERROR("oven", "oven_configure: failed to leave scratch area: %s\n", strerror(errno));
        return status;
    }

cleanup:
    __cleanup_backend_data(&data);
    return status;
}

static struct build_backend* __get_build_backend(const char* name)
{
    for (int i = 0; i < sizeof(g_buildbackends) / sizeof(struct build_backend); i++) {
        if (!strcmp(name, g_buildbackends[i].name)) {
            return &g_buildbackends[i];
        }
    }
    return NULL;
}

int oven_build(struct oven_build_options* options)
{
    struct build_backend*    backend;
    struct oven_backend_data data;
    int                      status;
    char*                    path;

    if (!options) {
        errno = EINVAL;
        return -1;
    }

    backend = __get_build_backend(options->system);
    if (!backend) {
        errno = ENOSYS;
        return -1;
    }

    // check if we already have done this step
    if (oven_checkpoint_contains(g_oven.recipe.scratch.host_checkpoint_path, options->name)) {
        printf("nothing to be done for %s\n", options->name);
        return 0;
    }

    printf("running step %s\n", options->name);
    status = __initialize_backend_data(&data, options->profile, options->arguments, options->environment);
    if (status) {
        return status;
    }

    status = scratch_enter(&g_oven.recipe.scratch);
    if (status) {
        VLOG_ERROR("oven", "oven_build: failed to enter scratch area: %s\n", strerror(errno));
        return status;
    }

    status = backend->build(&data, options->system_options);
    if (status == 0) {
        status = oven_checkpoint_create(g_oven.recipe.scratch.host_checkpoint_path, options->name);
    }

    status = scratch_leave(&g_oven.recipe.scratch);
    if (status) {
        VLOG_ERROR("oven", "oven_build: failed to leave scratch area: %s\n", strerror(errno));
        return status;
    }

cleanup:
    __cleanup_backend_data(&data);
    return status;
}

int oven_script(struct oven_script_options* options)
{
    const char* preprocessedScript;
    int         status;

    // handle script substitution first, then we pass it on
    // to the platform handler
    if (options == NULL || options->script == NULL) {
        errno = EINVAL;
        return -1;
    }

    // check if we already have done this step
    if (oven_checkpoint_contains(g_oven.recipe.scratch.host_checkpoint_path, options->name)) {
        printf("nothing to be done for %s\n", options->name);
        return 0;
    }

    printf("running step %s\n", options->name);
    preprocessedScript = oven_preprocess_text(options->script);
    if (preprocessedScript == NULL) {
        return -1;
    }

    status = scratch_enter(&g_oven.recipe.scratch);
    if (status) {
        VLOG_ERROR("oven", "oven_script: failed to enter scratch area: %s\n", strerror(errno));
        return status;
    }

    status = platform_script(preprocessedScript);
    if (status) {
        VLOG_ERROR("oven", "oven_script: failed to execute script\n");
    }
    free((void*)preprocessedScript);

    status = scratch_leave(&g_oven.recipe.scratch);
    if (status) {
        VLOG_ERROR("oven", "oven_script: failed to leave scratch area: %s\n", strerror(errno));
        return status;
    }

    if (status == 0) {
        status = oven_checkpoint_create(g_oven.recipe.scratch.host_checkpoint_path, options->name);
    }
    return status;
}

static int __matches_filters(const char* path, struct list* filters)
{
    struct list_item* item;

    if (filters->count == 0) {
        return 0; // YES! no filters means everything matches
    }

    list_foreach(filters, item) {
        struct oven_value_item* filter = (struct oven_value_item*)item;
        if (strfilter(filter->value, path, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

int __copy_files_with_filters(const char* sourceRoot, const char* path, struct list* filters, const char* destinationRoot)
{
    // recursively iterate through the directory and copy all files
    // as long as they match the list of filters
    int            status = -1;
    struct dirent* entry;
    DIR*           dir;
    const char*    finalSource;
    const char*    finalDestination = NULL;
    
    finalSource = strpathcombine(sourceRoot, path);
    if (!finalSource) {
        goto cleanup;
    }

    finalDestination = strpathcombine(destinationRoot, path);
    if (!finalDestination) {
        goto cleanup;
    }

    dir = opendir(finalSource);
    if (!dir) {
        goto cleanup;
    }

    // make sure target is created
    if (platform_mkdir(finalDestination)) {
        goto cleanup;
    }

    while ((entry = readdir(dir)) != NULL) {
        const char* combinedSubPath;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        combinedSubPath = strpathcombine(path, entry->d_name);
        if (!combinedSubPath) {
            goto cleanup;
        }

        // does this match filters?
        if (__matches_filters(combinedSubPath, filters)) {
            free((void*)combinedSubPath);
            continue;
        }

        // oh ok, is it a directory?
        if (entry->d_type == DT_DIR) {
            status = __copy_files_with_filters(sourceRoot, combinedSubPath, filters, destinationRoot);
            free((void*)combinedSubPath);
            if (status) {
                goto cleanup;
            }
        } else {
            // ok, it's a file, copy it
            char* sourceFile      = strpathcombine(finalSource, entry->d_name);
            char* destinationFile = strpathcombine(finalDestination, entry->d_name);
            free((void*)combinedSubPath);
            if (!sourceFile || !destinationFile) {
                free((void*)sourceFile);
                goto cleanup;
            }

            status = platform_copyfile(sourceFile, destinationFile);
            free((void*)sourceFile);
            free((void*)destinationFile);
            if (status) {
                goto cleanup;
            }
        }
    }

    closedir(dir);
    status = 0;

cleanup:
    free((void*)finalSource);
    free((void*)finalDestination);
    return status;
}

int oven_include_filters(struct list* filters)
{
    if (!filters) {
        errno = EINVAL;
        return -1;
    }

    return __copy_files_with_filters(
        "",
        NULL,
        filters,
        g_oven.recipe.scratch.install_root
    );
}
