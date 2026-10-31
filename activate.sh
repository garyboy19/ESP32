#!/usr/bin/env bash
# 在当前 shell 中激活本目录的 ESP-IDF 环境：
#   source ./activate.sh
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/export.sh/activate_idf_v6.1.sh"
