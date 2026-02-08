#!/bin/bash

MODES=("performance" "balanced" "power-saver")
ICONS=("⚡ Perf" "⚖ Bal" "🌱 Save")

current=$(powerprofilesctl get 2>/dev/null | tr -d '[:space:]')

current_index=-1
for i in "${!MODES[@]}"; do
    if [[ "${MODES[$i]}" == "$current" ]]; then
        current_index=$i
        break
    fi
done

if [[ "$1" == "toggle" ]]; then
    if [[ $current_index -ge 0 ]]; then
        next_index=$(( (current_index + 1) % ${#MODES[@]} ))
        powerprofilesctl set "${MODES[$next_index]}" 2>/dev/null
        
        # 关键：等待状态真正更新
        sleep 0.3
        
        # 重新读取更新后的状态
        current=$(powerprofilesctl get 2>/dev/null | tr -d '[:space:]')
    fi
fi

case "$current" in
    "performance")
        echo "⚡ Perf"
        ;;
    "balanced")
        echo "⚖ Bal"
        ;;
    "power-saver")
        echo "🌱 Save"
        ;;
    *)
        echo "❓"
        ;;
esac
