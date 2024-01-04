/**
 * Copyright 2024, Philip Meulengracht
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

#include <errno.h>
#include <chef/kitchen.h>
#include <chef/platform.h>
#include <fcntl.h>
#include <libingredient.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vlog.h>

static char* __string_array_join(const char* const* items, const char* prefix, const char* separator)
{
    char* buffer;

    buffer = calloc(4096, 1); 
    if (buffer == NULL) {
        return NULL;
    }

    for (int i = 0; items[i]; i++) {
        if (buffer[0] == 0) {
            strcpy(buffer, prefix);
        } else {
            strcat(buffer, separator);
            strcat(buffer, prefix);
        }
        strcat(buffer, items[i]);
    }
    return buffer;
}


// <root>/.oven/output
// <root>/.oven/<package>/bin
// <root>/.oven/<package>/lib
// <root>/.oven/<package>/share
// <root>/.oven/<package>/usr/...
// <root>/.oven/<package>/target/
// <root>/.oven/<package>/target/ingredients
// <root>/.oven/<package>/chef/build
// <root>/.oven/<package>/chef/install => <root>/.oven/output
// <root>/.oven/<package>/chef/project => <root>
static int __make_available(const char* hostRoot, const char* root, struct ingredient* ingredient)
{
    FILE* file;
    char  pcName[256];
    char* pcPath;
    int   written;
    int   status;
    char* cflags;
    char* libs;

    if (ingredient->options == NULL) {
        // Can't add a pkg-config file if the ingredient didn't specify any
        // options for consumers.
        // TODO: Add defaults?
        return 0;
    }

    // The package name specified on the pkg-config command line is defined 
    // to be the name of the metadata file, minus the .pc extension. Optionally
    // the version can be appended as name-1.0
    written = snprintf(&pcName[0], sizeof(pcName) - 1, "%s.pc", ingredient->package->package);
    if (written == (sizeof(pcName) - 1)) {
        errno = E2BIG;
        return -1;
    }
    
    pcPath = strpathjoin(hostRoot, "/usr/share/pkgconfig/", &pcName[0], NULL);
    if (pcPath == NULL) {
        return -1;
    }
    
    file = fopen(pcPath, "w");
    if(!file) {
        VLOG_ERROR("oven", "__make_available: failed to open %s for writing: %s\n", pcPath, strerror(errno));
        free(pcPath);
        return -1;
    }

    cflags = __string_array_join((const char* const*)ingredient->options->inc_dirs, "-I{prefix}", " ");
    libs = __string_array_join((const char* const*)ingredient->options->lib_dirs, "-L{prefix}", " ");
    if (cflags == NULL || libs == NULL) {
        free(cflags);
        free(libs);
        fclose(file);
        return -1;
    }

    fprintf(file, "# generated by chef, please do not manually modify this\n");
    fprintf(file, "prefix=%s\n", root);

    fprintf(file, "Name: %s\n", ingredient->package->package);
    fprintf(file, "Description: %s by %s\n", ingredient->package->package, ingredient->package->publisher);
    fprintf(file, "Version: %i.%i.%i\n", ingredient->version->major, ingredient->version->minor, ingredient->version->patch);
    fprintf(file, "Cflags: %s\n", cflags);
    fprintf(file, "Libs: %s\n", libs);
    free(cflags);
    free(libs);
    return fclose(file);
}

static int __setup_ingredients(struct kitchen* kitchen, struct list* ingredients)
{
    struct list_item* i;
    int               status;

    if (ingredients == NULL) {
        return 0;
    }

    list_foreach(ingredients, i) {
        struct oven_ingredient* ovenIngredient = (struct oven_ingredient*)i;
        struct ingredient*      ingredient;
        const char*             targetPath = "";
        const char*             hostTargetPath = kitchen->host_chroot;

        status = ingredient_open(ovenIngredient->file_path, &ingredient);
        if (status) {
            VLOG_ERROR("oven", "__setup_ingredients: failed to open %s\n", ovenIngredient->name);
            return -1;
        }

        // If the ingredient has a different platform or arch than host
        // then the ingredient should be installed differently
        if (strcmp(ingredient->package->platform, CHEF_PLATFORM_STR) ||
            strcmp(ingredient->package->arch, CHEF_ARCHITECTURE_STR)) {
            targetPath = kitchen->target_ingredients_path;
            hostTargetPath = kitchen->host_target_ingredients_path;
        }

        status = ingredient_unpack(ingredient, targetPath, NULL, NULL);
        if (status) {
            ingredient_close(ingredient);
            VLOG_ERROR("oven", "__setup_ingredients: failed to setup %s\n", ovenIngredient->name);
            return -1;
        }

        status = __make_available(hostTargetPath, targetPath, ingredient);
        ingredient_close(ingredient);
        if (status) {
            VLOG_ERROR("oven", "__setup_ingredients: failed to make %s available\n", ovenIngredient->name);
            return -1;
        }
    }
    return 0;
}

static char* __build_include_string(struct list* imports)
{
    struct list_item* i;
    char*             buffer;

    // --include=nano,gcc,clang,tcc,pcc,g++,git,make
    if (imports == NULL || imports->count == 0) {
        return NULL;
    }

    buffer = calloc(4096, 1); 
    if (buffer == NULL) {
        return NULL;
    }

    list_foreach(imports, i) {
        struct oven_package_import* import = (struct oven_package_import*)i;
        if (buffer[0] == 0) {
            strcpy(buffer, "--include=");
            strcat(buffer, import->name);
        } else {
            strcat(buffer, ",");
            strcat(buffer, import->name);
        }
    }
    return buffer;
}

static unsigned int __hash(unsigned int hash, const char* data, size_t length)
{
    for (unsigned int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// hash of ingredients and imports
static unsigned int __setup_hash(struct kitchen_options* options)
{
    unsigned int      hash = 5381;
    struct list_item* i;

    // hash name
    hash = __hash(hash, options->name, strlen(options->name));

    // hash ingredients
    if (options->ingredients != NULL) {
        list_foreach(options->ingredients, i) {
            struct oven_ingredient* ovenIngredient = (struct oven_ingredient*)i;
            hash = __hash(hash, ovenIngredient->name, strlen(ovenIngredient->name));
        }
    }
    
    // hash imports
    if (options->imports != NULL) {
        list_foreach(options->imports, i) {
            struct oven_package_import* import = (struct oven_package_import*)i;
            hash = __hash(hash, import->name, strlen(import->name));
        }
    }
    return hash;
}

static unsigned int __read_hash(const char* name)
{
    char  buff[512];
    FILE* hashFile;
    long  size;
    char* end = NULL;
    VLOG_TRACE("oven", "__read_hash()\n");

    snprintf(&buff[0], sizeof(buff), ".oven/%s/chef/.hash", name);
    hashFile = fopen(&buff[0], "r");
    if (hashFile == NULL) {
        VLOG_TRACE("oven", "__read_hash: no hash file\n");
        return 0;
    }

    fseek(hashFile, 0, SEEK_END);
    size = ftell(hashFile);
    rewind(hashFile);

    if (size >= sizeof(buff)) {
        VLOG_ERROR("oven", "__read_hash: the hash file was invalid\n");
        fclose(hashFile);
        return 0;
    }
    if (fread(&buff[0], 1, size, hashFile) < size) {
        VLOG_ERROR("oven", "__read_hash: failed to read hash file\n");
        fclose(hashFile);
        return 0;
    }
    
    fclose(hashFile);
    return (unsigned int)strtoul(&buff[0], &end, 10);
}

static int __write_hash(struct kitchen_options* options)
{
    char         kitchenPad[512];
    FILE*        hashFile;
    unsigned int hash;
    VLOG_TRACE("oven", "__write_hash(name=%s)\n", options->name);

    snprintf(&kitchenPad[0], sizeof(kitchenPad), ".oven/%s/chef/.hash", options->name);
    hashFile = fopen(&kitchenPad[0], "w");
    if (hashFile == NULL) {
        VLOG_TRACE("oven", "__read_hash: no hash file");
        return 0;
    }

    hash = __setup_hash(options);
    fprintf(hashFile, "%u", hash);
    fclose(hashFile);
    return 0;
}

static int __should_skip_setup(struct kitchen_options* options)
{
    unsigned int currentHash  = __setup_hash(options);
    unsigned int existingHash = __read_hash(options->name);
    return currentHash == existingHash;
}

static int __kitchen_construct(struct kitchen_options* options, struct kitchen* kitchen)
{
    char buff[512];
    VLOG_DEBUG("oven", "__kitchen_construct(name=%s)\n", options->name);

    snprintf(&buff[0], sizeof(buff), ".oven/%s", options->name);
    kitchen->host_chroot = strdup(&buff[0]);

    snprintf(&buff[0], sizeof(buff), ".oven/%s/target/ingredients", options->name);
    kitchen->host_target_ingredients_path = strdup(&buff[0]);

    snprintf(&buff[0], sizeof(buff), ".oven/%s/chef/build", options->name);
    kitchen->host_build_path = strdup(&buff[0]);

    snprintf(&buff[0], sizeof(buff), ".oven/%s/chef/install", options->name);
    kitchen->host_install_path = strdup(&buff[0]);

    snprintf(&buff[0], sizeof(buff), ".oven/%s/chef/.checkpoint", options->name);
    kitchen->host_checkpoint_path = strdup(&buff[0]);

    kitchen->target_ingredients_path = strdup("/target/ingredients");
    kitchen->project_root = strdup("/chef/project");
    kitchen->build_root = strdup("/chef/build");
    kitchen->install_root = strdup("/chef/install");
    kitchen->confined = options->confined;
    return 0;
}

int kitchen_setup(struct kitchen_options* options, struct kitchen* kitchen)
{
    char  buff[512];
    char* includes;
    int   status;
    VLOG_DEBUG("oven", "kitchen_setup(name=%s)\n", options->name);

    if (__should_skip_setup(options)) {
        return __kitchen_construct(options, kitchen);
    }

    snprintf(&buff[0], sizeof(buff), ".oven/%s/target/ingredients", options->name);
    if (platform_mkdir(&buff[0])) {
        VLOG_ERROR("oven", "kitchen_setup: failed to create %s\n", &buff[0]);
        return -1;
    }

    snprintf(&buff[0], sizeof(buff), ".oven/%s/chef/build", options->name);
    if (platform_mkdir(&buff[0])) {
        VLOG_ERROR("oven", "kitchen_setup: failed to create %s\n", &buff[0]);
        return -1;
    }

    snprintf(&buff[0], sizeof(buff), ".oven/%s/chef/install", options->name);
    if (platform_symlink(&buff[0], options->install_path, 1)) {
        VLOG_ERROR("oven", "kitchen_setup: failed to link %s\n", &buff[0]);
        return -1;
    }

    snprintf(&buff[0], sizeof(buff), ".oven/%s/chef/project", options->name);
    if (platform_symlink(&buff[0], options->project_path, 1)) {
        VLOG_ERROR("oven", "kitchen_setup: failed to link %s\n", &buff[0]);
        return -1;
    }

    if (__kitchen_construct(options, kitchen)) {
        return -1;
    }

    // extract os/ingredients/toolchain
    if (__setup_ingredients(kitchen, options->ingredients)) {
        return -1;
    }

    // write hash
    if (__write_hash(options)) {
        return -1;
    }
    return 0;
}

int kitchen_enter(struct kitchen* kitchen)
{
    VLOG_DEBUG("oven", "kitchen_enter(confined=%i)\n", kitchen->confined);
    
    if (!kitchen->confined) {
        // for an unconfined we do not chroot, instead we allow full access
        // to the base operating system to allow the the part to include all
        // it needs.
        return 0;
    }

    if (kitchen->original_root_fd > 0) {
        VLOG_ERROR("oven", "kitchen_enter: cannot recursively enter kitchen root\n");
        return -1;
    }

    kitchen->original_root_fd = open("/", __O_PATH);
    if (kitchen->original_root_fd < 0) {
        VLOG_ERROR("oven", "kitchen_enter: failed to get a handle on root: %s\n", strerror(errno));
        return -1;
    }

    if (chroot(kitchen->host_chroot)) {
        VLOG_ERROR("oven", "kitchen_enter: failed to change root environment to %s\n", kitchen->host_chroot);
        return -1;
    }

    // Change working directory to the known project root
    if (chdir(kitchen->project_root)) {
        VLOG_ERROR("oven", "kitchen_enter: failed to change working directory to %s\n", kitchen->project_root);
        return -1;
    }
    return 0;
}

int kitchen_leave(struct kitchen* kitchen)
{
    VLOG_DEBUG("oven", "kitchen_leave()\n");

    if (!kitchen->confined) {
        // nothing to do for unconfined
        return 0;
    }
    
    if (kitchen->original_root_fd <= 0) {
        return -1;
    }

    if (fchdir(kitchen->original_root_fd)) {
        return -1;
    }
    if (chroot(".")) {
        return -1;
    }
    close(kitchen->original_root_fd);
    kitchen->original_root_fd = 0;
    return 0;
}
