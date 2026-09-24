#!/bin/bash
#
# dryrun.sh - crysbox / negpid 安装预演（带返回/退出）
#

set -u

PROJECT_REPO="${PROJECT_REPO:-https://github.com/noobcjk/Crysbox.git}"
PROJECT_TAG="${PROJECT_TAG:-master}"
PROJECT_ROOT="${PROJECT_ROOT:-/tmp/Crysbox}"

BACKUP_DIR="${BACKUP_DIR:-$HOME/.crysbox-backup}"
INSTALL_RECORD="${INSTALL_RECORD:-$HOME/.crysbox-installed}"

DIR_BOX="Crysbox"
DIR_NEGPID="NEGPID"

log()  { echo -e "\033[1;32m[dryrun]\033[0m $*"; }
warn() { echo -e "\033[1;33m[dryrun]\033[0m $*"; }
err()  { echo -e "\033[1;31m[dryrun]\033[0m $*"; }
die()  { err "$*"; exit 1; }

# ---------- 读内核版本 ----------

read_kernel_version() {
	local v
	v=$(make kernelversion 2>/dev/null)
	if [ -n "$v" ]; then
		echo "$v"; return
	fi
	local V P S
	V=$(grep -E '^VERSION[[:space:]]*=' Makefile | head -n1 | sed 's/.*=[[:space:]]*//' | tr -d ' ')
	P=$(grep -E '^PATCHLEVEL[[:space:]]*=' Makefile | head -n1 | sed 's/.*=[[:space:]]*//' | tr -d ' ')
	S=$(grep -E '^SUBLEVEL[[:space:]]*=' Makefile | head -n1 | sed 's/.*=[[:space:]]*//' | tr -d ' ')
	echo "$V.$P.$S"
}

# ---------- 拉项目 ----------

fetch_project() {
	if [ -d "$PROJECT_ROOT/.git" ]; then
		log "项目已存在，git pull: $PROJECT_ROOT"
		(
			cd "$PROJECT_ROOT"
			git fetch --all
			if ! git reset --hard "origin/$PROJECT_TAG" 2>/dev/null; then
				if ! git reset --hard "$PROJECT_TAG" 2>/dev/null; then
					git pull
				fi
			fi
		)
	else
		log "克隆项目 $PROJECT_REPO -> $PROJECT_ROOT"
		rm -rf "$PROJECT_ROOT"
		git clone "$PROJECT_REPO" "$PROJECT_ROOT" || die "克隆失败"
	fi
	[ -d "$PROJECT_ROOT" ] || die "项目目录不存在: $PROJECT_ROOT"
}

# ---------- 支持列表 ----------

list_supported_kernels() {
	{
		find "$PROJECT_ROOT/$DIR_BOX"   -mindepth 2 -maxdepth 2 -type d -printf '%f\n' 2>/dev/null
		find "$PROJECT_ROOT/$DIR_NEGPID" -mindepth 2 -maxdepth 2 -type d -printf '%f\n' 2>/dev/null
	} | sort -uV
}

# ---------- 安装记录 ----------

list_installed_for_kernel() {
	local kver="$1"
	[ -f "$INSTALL_RECORD" ] || return
	awk -v k="$kver" '$3 == k {print $1, $2, $3}' "$INSTALL_RECORD"
}

# ---------- 选择器 ----------

PICK_RESULT=""
pick_menu() {
	local prompt="$1"
	shift
	local arr=("$@")

	if [ ${#arr[@]} -eq 0 ]; then
		return 4
	fi

	echo "$prompt"
	local i=1
	for v in "${arr[@]}"; do
		printf "  %2d) %s\n" "$i" "$v"
		i=$((i+1))
	done
	printf "   0) 返回上一层\n"
	printf "   q) 退出\n"
	echo -n "请选择 [1-$((i-1))/0/q]: "
	read -r sel

	case "$sel" in
	0) return 2 ;;
	q|Q) return 3 ;;
	esac

	if ! [ "$sel" -ge 1 ] 2>/dev/null || [ "$sel" -ge "$i" ]; then
		return 1
	fi

	PICK_RESULT="${arr[$((sel-1))]}"
	return 0
}

list_subdirs() {
	local d="$1"
	[ -d "$d" ] || return
	find "$d" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -V
}

# ---------- 主流程 ----------

main() {
	KERNEL_DIR="$(pwd)"

	echo "========================================"
	echo "  crysbox / negpid 安装预演"
	echo "========================================"
	echo ""

	# 1. 内核目录
	if [ ! -f "Makefile" ] || [ ! -d "fs/proc" ] || [ ! -f "kernel/pid.c" ]; then
		die "当前目录不是内核源码目录: $KERNEL_DIR"
	fi
	log "内核源码目录: $KERNEL_DIR"

	# 2. 读版本
	KVER=$(read_kernel_version)
	[ -n "$KVER" ] || die "无法读取内核版本"

	if [ -f "vmlinux" ] && [ -f "System.map" ]; then
		BUILT="已构建"
	else
		BUILT="未构建"
	fi

	# 3. 拉项目
	fetch_project
	echo ""

	# 4. 判断是否受支持
	local SUPPORTED_KERNELS
	SUPPORTED_KERNELS=$(list_supported_kernels)
	SUPPORTED=0
	for kv in $SUPPORTED_KERNELS; do
		if [ "$kv" = "$KVER" ]; then
			SUPPORTED=1; break
		fi
	done

	echo "========================================"
	if [ "$SUPPORTED" = "1" ]; then
		echo " 当前版本: $KVER  | \033[1;32m受支持\033[0m"
	else
		echo " 当前版本: $KVER  | \033[1;31m不受支持\033[0m"
	fi
	echo " 内核状态: $BUILT"
	echo " 项目目录: $PROJECT_ROOT"
	if [ -n "$SUPPORTED_KERNELS" ]; then
		echo " 支持的版本:"
		for v in $SUPPORTED_KERNELS; do
			if [ "$v" = "$KVER" ]; then
				echo "   - $v  (当前)"
			else
				echo "   - $v"
			fi
		done
	fi
	echo "========================================"
	echo ""

	if [ "$SUPPORTED" = "0" ]; then
		warn "不受支持的版本 $KVER"
		echo -n "你确定要继续吗? [y/N]: "
		read -r ans
		case "$ans" in
		y|Y) log "继续..." ;;
		*)   die "用户取消" ;;
		esac
		echo ""
	fi

	# 5. 判断是否安装过
	local installed
	installed=$(list_installed_for_kernel "$KVER")
	if [ -n "$installed" ]; then
		warn "检测到当前内核 $KVER 已安装过:"
		echo "$installed" | while read -r f t k; do
			echo "   - $f $t $k"
		done
		echo ""
		echo -n "是否继续安装? [y/N]: "
		read -r ans
		case "$ans" in
		y|Y) log "继续..." ;;
		*)   die "用户取消" ;;
		esac
		echo ""
	fi

	# ==================== 状态机 ====================
	local STATE="feature"
	local RET=0
	local FEATURE="" FEATURE_DIR="" TOOLVER="" TARGET_KVER=""

	while true; do
		case "$STATE" in

		feature)
			pick_menu "请选择功能:" "Crysbox" "NEGPID"
			RET=$?
			case $RET in
			0)  FEATURE="$PICK_RESULT"; FEATURE_DIR="$PROJECT_ROOT/$FEATURE"; STATE="toolver" ;;
			2)  STATE="quit" ;;
			3)  STATE="quit" ;;
			*)  ;; # 1/4 非法/空，重试
			esac
			;;

		toolver)
			mapfile -t TOOL_VERSIONS < <(list_subdirs "$FEATURE_DIR")
			pick_menu "请选择工具版本:" "${TOOL_VERSIONS[@]}"
			RET=$?
			case $RET in
			0)  TOOLVER="$PICK_RESULT"; STATE="kver" ;;
			2)  STATE="feature" ;;
			3)  STATE="quit" ;;
			*)  ;;
			esac
			;;

		kver)
			mapfile -t KERNEL_VERSIONS < <(list_subdirs "$FEATURE_DIR/$TOOLVER")
			pick_menu "请选择内核版本:" "${KERNEL_VERSIONS[@]}"
			RET=$?
			case $RET in
			0)  TARGET_KVER="$PICK_RESULT"; STATE="summary" ;;
			2)  STATE="toolver" ;;
			3)  STATE="quit" ;;
			*)  ;;
			esac
			;;

		summary)
			echo ""
			echo "========================================"
			echo " 功能:     $FEATURE"
			echo " 工具版本: $TOOLVER"
			echo " 目标内核: $TARGET_KVER"
			echo " 当前内核: $KVER"
			echo " 源目录:   $FEATURE_DIR/$TOOLVER/$TARGET_KVER"
			echo "========================================"
			echo ""
			printf "  1) 继续\n"
			printf "  0) 返回上一层\n"
			printf "  q) 退出\n"
			echo -n "请选择 [1/0/q]: "
			read -r sel
			case "$sel" in
			1)  STATE="proceed" ;;
			0)  STATE="kver" ;;
			q|Q) STATE="quit" ;;
			*)  ;;
			esac
			;;

		proceed)
			log "开始执行..."
			# check_symbols / dryrun_box / dryrun_negpid 接这里
			log "========== 预演结束（未修改任何文件）=========="
			STATE="done"
			;;

		done)
			break
			;;

		quit|*)
			log "用户取消"
			exit 0
			;;
		esac
	done
}

main "$@"