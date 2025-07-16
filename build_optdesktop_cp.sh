#!/bin/bash
cd obj-b2g-optdesktop
NUTRIA_PATH="/home/gty//source/my_nutria"
cp dist/bin/b2g $NUTRIA_PATH/prebuilts/b2g/
cp dist/bin/crashreporter $NUTRIA_PATH/prebuilts/b2g/
cp security/nss/lib/freebl/freebl_freeblpriv3/libfreeblpriv3.so $NUTRIA_PATH/prebuilts/b2g/
cp config/external/gkcodecs/libgkcodecs.so $NUTRIA_PATH/prebuilts/b2g/
cp config/external/lgpllibs/liblgpllibs.so $NUTRIA_PATH/prebuilts/b2g/
cp widget/gtk/mozgtk/libmozgtk.so $NUTRIA_PATH/prebuilts/b2g/
cp config/external/sqlite/libmozsqlite3.so $NUTRIA_PATH/prebuilts/b2g/
cp widget/gtk/mozwayland/libmozwayland.so $NUTRIA_PATH/prebuilts/b2g/
cp config/external/nspr/pr/libnspr4.so $NUTRIA_PATH/prebuilts/b2g/
cp security/nss/lib/nss/nss_nss3/libnss3.so $NUTRIA_PATH/prebuilts/b2g/
cp security/manager/ssl/builtins/dynamic-library/libnssckbi.so $NUTRIA_PATH/prebuilts/b2g/
cp security/nss/lib/util/util_nssutil3/libnssutil3.so $NUTRIA_PATH/prebuilts/b2g/
cp config/external/nspr/libc/libplc4.so $NUTRIA_PATH/prebuilts/b2g/
cp config/external/nspr/ds/libplds4.so $NUTRIA_PATH/prebuilts/b2g/
cp security/nss/lib/smime/smime_smime3/libsmime3.so $NUTRIA_PATH/prebuilts/b2g/
cp security/nss/lib/softoken/softoken_softokn3/libsoftokn3.so $NUTRIA_PATH/prebuilts/b2g/
cp security/nss/lib/ssl/ssl_ssl3/libssl3.so $NUTRIA_PATH/prebuilts/b2g/
cp dist/bin/libxul.so $NUTRIA_PATH/prebuilts/b2g/
cp dist/bin/plugin-container $NUTRIA_PATH/prebuilts/b2g/
cp dist/bin/xpcshell $NUTRIA_PATH/prebuilts/b2g/

