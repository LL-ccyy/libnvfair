#!/bin/bash
# clangd wrapper:Trae server 把自带旧 libstdc++(GLIBCXX 仅到 3.4.25)注入
# LD_LIBRARY_PATH,导致系统 clangd 14(需 3.4.29+)启动即崩。
# 剥掉污染路径后用系统运行时库启动真实 clangd。
exec env -u LD_LIBRARY_PATH /usr/bin/clangd "$@"
