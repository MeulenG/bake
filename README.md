# About
Originally developed for the Vali/MollenOS operating system, this is a generic package management system that is built as a lightweight alternative to current package managers. Its not only for package management, but also as an application format. 

Chef consists of 3 parts, bake, order and serve.

[![Get it from the Snap Store](https://snapcraft.io/static/images/badges/en/snap-store-black.svg)](https://snapcraft.io/vchef)
[![vchef](https://snapcraft.io/vchef/badge.svg)](https://snapcraft.io/vchef)

## Bake
The bake utility serves as the builder, and orchestrates everything related to generation of bake packages. Bake packages serve both as packages and application images that can be executed by serve.

## Order
Order handles the orchestration of the online segment. Order controls your account setup, downloading of packages, package query and is the gateway to serve

## Serve
Serve consts of a frontend and a backend. The frontend is a CLI utility that allows you to interact with the backend. The backend is the application backend. This needs to be implemented on an OS basis. View the README located under daemons/served for more information.

Serve and Served communicate using the serve protocol, the serve protocol is implemented using the Gracht library.

## Recipe Specification

```
#########################
# project
#
# This member is required, and specifies project information which can be
# viewed with 'order info'.
project:
  ###########################
  # summary - Required
  #
  # A short summary of the project, this will be shown in the first line
  # of the project info page.
  summary: Simple Application Recipe

  ###########################
  # description - Required
  #
  # A longer description of the project, detailing what the purpose is and how
  # to use it.
  description: A simple application recipe

  ###########################
  # author - Required
  #
  # The project author(s), this is just treated as a string value.
  author: who made it

  ###########################
  # email - Required
  #
  # The email of the project or the primary author/maintainer.
  # This will be visible to anyone who downloads the package.
  email: contact@me.com

  ##########################
  # version - Required
  #
  # A three part version number for the current project version. Chef
  # automatically adds an auto-incrementing revision number. This means
  # for every publish done the revision increments, no matter if the 
  # version number stays the same. 
  version: 0.1.0
  
  #########################
  # icon - Optional
  #
  # The project icon file. This is either a png, bmp or jpg file that will be
  # shown in the project info page, or in the application launcher.
  icon: /path/to/icon.png
  
  #########################
  # license - Optional
  #
  # Specify the project license, this can either be a short-form of know
  # licenses or a http link to the project license if a custom one is used.
  license: MIT
  
  #########################
  # eula - Optional
  #
  # If provided, the chef will open and require the user to sign an eula
  # in case one if required for installing the package. <Planned Feature>
  # The signing will be done either in the CLI or in the GUI when it arrives.
  eula: https://myorg.com/project-eula

  #########################
  # homepage - Optional
  #
  # The project website, it is expected for this to be an url if provided.
  homepage:

###########################
# ingredients - Optional
#
# Ingredients are the same as dependencies. They are either
# libraries or toolchains the project needs to build correctly.
# Ingredients are unpacked to ${{ INGREDIENTS_PREFIX }}
# Toolchains are unpacked to ${{ TOOLCHAIN_PREFIX }}. Only one
# toolchain can be used per recipe. 
ingredients:
    ###########################
    # name - Required
    # 
    # Name of the ingredient required. How the name is given depends on the source
    # the package comes from. If the ingredient is a chef-package, then it must be
    # given in the format publisher/package.
  - name: vali/package
    
    ###########################
    # version - Optional
    #
    # A specific version can be given, this will attempt to resolve the package
    # with the wanted version, if no version is provided, then the latest will be
    # fetched.
    # Supported version formats:
    #  - <major>.<minor>.<patch>
    #  - <revision>
    version: 1.0.1

    ###########################
    # include - Optional
    #    values: {false, true}
    #
    # Specifies the ingredient should be bundled into the output
    # of this package build. This is used to include runtime dependencies for
    # applications, or to build aggregate packages. The default value for this
    # is false.
    include: false
    
    ###########################
    # include-filters - Optional
    #
    # Array of filters that should be used to filter files from this ingredient.
    # This can only be used in conjungtion with 'include: true', and exclusion
    # filters can be set by prefixing with '!'
    include-filters:
      - bin/*.dll
      - lib/*.lib
      - !share

    ###########################
    # platform - Optional
    #
    # The platform configuration of the package to retrieve. This is usefull
    # if cross-compiling for another platform. The default value for this is
    # the host platform.
    platform: linux

    ###########################
    # channel - Optional
    #
    # The channel to retrieve the package from. The default channel to retrieve
    # packages from is 'stable'.
    channel: stable

    ###########################
    # arch - Optional
    #    values: {i386, amd64, arm, arm64, rv32, rv64}
    #
    # The architecture configuration of the package to retrieve. This is also usefull
    # for cross-compiling for other architectures. This value defaults to host architecture.
    arch: amd64

    ###########################
    # description - Optional
    #
    # Provides a description for why this ingredient is included in the project.
    description: A library

    ###########################
    # source - Optional
    #
    # Specifies where the package should be retrieved from. If no source is provided
    # the ingredient will be fetched from the chef package repository.
    source:
      ###########################
      # type - Required
      #    values: {local, url, repo}
      #
      # 'repo' is the default value, and has no extra parameters in this.
      # 'local' can be specified for a local file, and has 'path' extra parameter
      # 'url' can be specified for downloading a file, and has 'url' extra parameter.
      type: repo

###########################
# recipes - Required
#
# Recipes describe how to build up all components of this project. A project
# can consist of multiple recipes, that all make up the final product.
recipes:
    ###########################
    # name - Required
    # 
    # Name of the recipe. This should be a very short name as it will
    # be used to scope the build files while building.
  - name: my-app
    
    ###########################
    # path - Optional
    # 
    # If the source code is not in the root directory, but in a project subfolder
    # then path can be used to specify where the root of source code of this recipe
    # is in relative terms from project root.
    path: source/

    ###########################
    # toolchain - Optional
    # 
    # If the recipe needs to be built using a specific toolchain this can be
    # specified here, this must refer to a package in 'ingredients'
    toolchain: vali/package

    ###########################
    # steps - Required
    #
    # Steps required to build the project. This usually involves
    # configuring, building and installing the project. Each generator backend
    # will automatically set the correct installation prefix when invoking the
    # generator.
    steps:
      
      ###########################
      # type - Required
      #    values: {generate, build}
      #
      # The step type, which must be specified. This determines which
      # kinds of 'system' is available for this step.
    - type: generate
      
      ###########################
      # system - Required
      #    generate-values: {autotools, cmake}
      #    build-values:    {make}
      #
      # This determines which backend will be used for this step. Configure steps
      # will only be invoked when they change (todo!), but build/install steps are always
      # executed.
      system: autotools

      ###########################
      # arguments - Optional
      # 
      # List of arguments that should be passed to the spawn invocation.
      arguments: [--arg=value]

      ###########################
      # env - Optional
      #
      # List of environment variables that should be passed to the spawn
      # invocations. This will override the inherited host variables if a
      # variable with the same key is specified on the host. 
      env:
        VAR: VALUE

packs:
  - name: mypack
    ###########################
    # type - Required
    #    values: {ingredient, application, toolchain}
    #
    # The project type, this defines how the pack is being used by the backend
    # when building projects that rely on this package. Toolchains will be unpacked
    # and treated differently than ingredients would. Only applications can be installed
    # by the application system, and should only contain the neccessary files to be installed,
    # while ingredients might contains headers, build files etc.
    type: application

    ###########################
    # filters - Optional
    #
    # Array of filters that should be used to filter files from the install path
    # exclusion filters can be set by prefixing with '!'
    filters:
      - bin/app
      - bin/*.dll
      - share
    
    ###########################
    # commands - Required for applications
    # 
    # commands are applications or services that should be available
    # to the system once the application is installed. These commands
    # can be registered to a binary or script inside the app package
    commands:
    - name: myapp
        path: /bin/myapp
        arguments: [--arg1, --arg2]
        type: executable
        description: A simple application
```
