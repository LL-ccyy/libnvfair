#!/usr/bin/env bash
# clangd-wrapper.sh — clangd 启动包装。
#
# 职责:仅清理 Trae 终端注入的坏 LD_LIBRARY_PATH(旧 libstdc++ 缺
# GLIBCXX_3.4.29,会让 clangd 秒崩)。所有参数原样透传。
# --compile-commands-dir 由 .vscode/settings.json 的 clangd.arguments 提供,
# 此处不得再传(clangd 限制该选项最多出现一次,重复即启动失败)。
#
# 子模块(ogkm)的跳转支持:build/compile_commands.json 已合并
# compile_commands_ogkm.json 的合成条目(escape.c 等);CMake 重新 configure
# 后需重跑合并,见该文件内注释。

exec env -u LD_LIBRARY_PATH clangd "$@"
