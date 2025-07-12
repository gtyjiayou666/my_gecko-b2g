#!/bin/bash

DIR=$(pwd)
GECKO_OBJDIR=${GECKO_OBJDIR:-objdir-gecko}
GONK_PATH=${GONK_PATH:-$DIR}

if [ -z "$GECKO_OBJDIR" -o ! -e "$GECKO_OBJDIR/config.log" ]; then
    echo "GECKO_OBJDIR is invalid!" >&2
    exit 255
fi

if [ -z "$GONK_PATH" -o ! -e "$GONK_PATH/prebuilts/gcc" ]; then
    echo "GONK_PATH is invalid!" >&2
    exit 255
fi

case "$TARGET_ARCH" in
    arm)
        TOOLS_PREFIX=$GONK_PATH/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
        ;;
    arm64)
        TOOLS_PREFIX=$GONK_PATH/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-
        ;;
esac

OBJCOPY=${TOOLS_PREFIX}objcopy
STRIP=${TOOLS_PREFIX}strip

cd $GECKO_OBJDIR/dist
DIST_DIR=$(pwd)

mkdir -p sysroot/system/b2g

cd bin
echo "Prepare debug files"
for obj in b2g *.so; do
    echo "    $obj"
    $OBJCOPY --only-keep-debug $obj $obj.dbg
    $OBJCOPY --remove-section ".gnu_debuglink" $obj
    $OBJCOPY --add-gnu-debuglink="$obj.dbg" $obj
    cp $obj $DIST_DIR/sysroot/system/b2g/
    $STRIP --strip-debug --strip-unneeded $DIST_DIR/sysroot/system/b2g/$obj
    mv $obj.dbg $DIST_DIR/sysroot/system/b2g/
done

if [ -z "$NO_PULL_SYSTEM" ]; then
    echo "Pull system object files"
    cd $DIST_DIR/sysroot/system
    adb pull /system/bin
    adb pull /system/lib

    eval $(grep DEVICE_NAME $GONK_PATH/.config)
    if [ "$DEVICE_NAME" = "modric_q_st" ]; then
        cd $DIST_DIR/sysroot
        adb pull /apex
        adb pull /vendor
    fi
fi
