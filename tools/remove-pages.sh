#!/usr/bin/env bash
#
# 删除已从菜单移除的页面 chunk
#
# 菜单（api/menu.json）不再注册这些路由后，对应的页面组件文件就是死代码。
# 本脚本把它们从后台 js 目录移走（默认移到备份目录，不留残骸）。
#
# 用法：
#   ./remove-pages.sh /www/wwwroot/站点/public/<后台目录>/js [备份目录]
#
# 不带备份目录则直接删除。

set -eu

ADMIN_JS="${1:?用法: remove-pages.sh <后台目录>/js [备份目录]}"
BACKUP="${2:-}"

PAGES=(
  # 用户空间
  userCenter-user-space
  # 客服
  Chat-Chatset
  Chat-DataAnalysis
  Chat-Message
  Chat-sava-DataAnalysis
  Chat-sava-Message
  # 反馈
  feedback-allfeedback
  feedback-allfeedback-fullpage
  feedback-submitfeedback
  # 授权 → 盗版举报
  authorization-piracyReport
)

if [ ! -d "$ADMIN_JS" ]; then
  echo "目录不存在: $ADMIN_JS" >&2
  exit 1
fi

if [ -n "$BACKUP" ]; then
  mkdir -p "$BACKUP"
fi

n=0
for p in "${PAGES[@]}"; do
  for f in "$ADMIN_JS/$p".*.js; do
    [ -e "$f" ] || continue
    if [ -n "$BACKUP" ]; then
      mv -f "$f" "$BACKUP"/
      echo "移走 $(basename "$f") → $BACKUP"
    else
      rm -f "$f"
      echo "删除 $(basename "$f")"
    fi
    n=$((n + 1))
  done
done

echo "共处理 $n 个文件"
