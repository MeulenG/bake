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

#include <errno.h>
#include <chef/api/account.h>
#include <chef/api/package.h>
#include <chef/client.h>
#include <libplatform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void account_setup(void);

static void __print_help(void)
{
    printf("Usage: order publish <pack-path> [options]\n");
    printf("Options:\n");
    printf("  -c, --channel\n");
    printf("      The channel that should be published to, default is devel\n");
    printf("  -h, --help\n");
    printf("      Print this help message\n");
}

static int __ensure_account_setup(void)
{
    struct chef_account* account;
    int                  status;

    status = chef_account_get(&account);
    if (status != 0) {
        if (status == -ENOENT) {
            printf("no account information available yet\n");
            account_setup();
            return 0;
        }
        return status;
    }

    // verify account status is active
    if (chef_account_get_status(account) != CHEF_ACCOUNT_STATUS_ACTIVE) {
        printf("account has been suspended, you are not allowed to publish new packages\n");
        errno = EACCES;
        return -1;
    }

    // verify publisher-name has been confirmed
    if (chef_account_get_verified_status(account) != CHEF_ACCOUNT_VERIFIED_STATUS_VERIFIED) {
        printf("publisher name has not been verified yet, please wait for verification status to be approved\n");
        errno = EACCES;
        return -1;
    }

    chef_account_free(account);
    return 0;
}

int publish_main(int argc, char** argv)
{
    struct chef_publish_params params   = { 0 };
    struct chef_package*       package  = NULL;
    struct chef_version*       version  = NULL;
    char*                      packPath = NULL;
    int                        status;

    // set default channel
    params.channel = "devel";

    if (argc > 2) {
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
                __print_help();
                return 0;
            } else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--channel")) {
                params.channel = argv[++i];
            } else {
                if (packPath != NULL) {
                    printf("only one pack path can be specified\n");
                    return -1;
                }

                packPath = argv[i];
            }
        }
    }

    // parse the pack for all the information we need
    if (packPath == NULL) {
        printf("no pack path specified\n");
        return -1;
    }

    status = chef_package_load(packPath, &package, &version);
    if (status != 0) {
        printf("failed to load package: %s\n", strerror(errno));
        return -1;
    }

    // dump information
    printf("publishing package: %s\n", package->package);
    printf("platform:           %s\n", package->platform);
    printf("architecture:       %s\n", package->arch);
    printf("channel:            %s\n", params.channel);
    printf("version:            %d.%d.%d\n", version->major, version->minor, version->patch);

    // set the parameter values
    params.package = package;
    params.version = version;

    // initialize chefclient
    status = chefclient_initialize();
    if (status != 0) {
        fprintf(stderr, "failed to initialize chefclient: %s\n", strerror(errno));
        return -1;
    }
    atexit(chefclient_cleanup);

    // do this in a loop, to catch cases where our login token has
    // expired
    while (1) {
        // login before continuing
        status = chefclient_login(CHEF_LOGIN_FLOW_TYPE_OAUTH2_DEVICECODE);
        if (status != 0) {
            printf("failed to login to chef server: %s\n", strerror(errno));
            break;
        }

        // ensure account is setup
        status = __ensure_account_setup();
        if (status != 0) {
            if (status == -EACCES) {
                chefclient_logout();
                continue;
            }
            printf("failed to setup neccessary account information: %s\n", strerror(-status));
            break;
        }

        // publish the package
        status = chefclient_pack_publish(&params, packPath);
        if (status != 0) {
            printf("failed to publish package: %s\n", strerror(errno));
            break;
        }

        printf("package published successfully\n");
        break;
    }
    
    chef_version_free(version);
    chef_package_free(package);
    return status;
}
