#!/bin/sh

set -eu

export PATH=~/android/toolchain/bin/:$PATH
export CC=arm-linux-androideabi-gcc

export ac_cv_func_malloc_0_nonnull=yes
export ac_cv_func_realloc_0_nonnull=yes

./configure --target=armv5 --host=x86_64-pc-linux-gnu --disable-selinux --disable-fsadm --disable-readline --enable-static_link --with-optimisation=-Os
make clean
make device-mapper
cp tools/dmsetup.static tools/dmsetup.stripped
arm-linux-androideabi-strip tools/dmsetup.stripped
