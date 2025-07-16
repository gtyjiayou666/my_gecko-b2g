#!/bin/bash

export MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none

export PATH=$HOME/.mozbuild/clang/bin:$PATH
rustup default 1.69.0


export MOZCONFIG=./mozconfig-b2g-optdesktop
export MOZ_OBJDIR=./obj-b2g-optdesktop/

./mach build $1
