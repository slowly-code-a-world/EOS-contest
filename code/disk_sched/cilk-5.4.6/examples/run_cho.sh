#!/bin/bash

sync
echo 3 > /proc/sys/vm/drop_caches

echo enable > /proc/fast_auto

time ./cholesky --nproc 256

echo disable > /proc/fast_auto
