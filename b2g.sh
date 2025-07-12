#!/bin/bash

rustup default 1.69.0


export MOZCONFIG=./mozconfig-b2g
export MOZ_OBJDIR=./obj-b2g/

./mach build $1
