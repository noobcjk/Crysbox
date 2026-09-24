#!/bin/bash
#
# install.sh - crysbox / negpid 安装
#
# 流程:
#   1. 判断内核目录
#   2. 读当前内核版本
#   3. 判断是否 make 过
#   4. git clone / pull -> /tmp/Crysbox
#   5. 判断是否受支持
#   6. 判断是否已安装
#   7. 选功能
#   8. 选工具版本
#   9. 选内核版本
#  10. 符号检测
#      - 有 STATIC/NOEXPORT 或 MISSING -> 拒绝
#      - 有 GLOBAL/NOEXPORT 或 STATIC/EXPORT -> 允许
#      - 全 OK -> 允许
#  11. 摘要
#  12. 执行
#

set -u

# ==================== 配置 ====================

PROJECT_REPO="${PROJECT_REPO:-https://github.com/yourname/crysbox.git}"
PROJECT_TAG="${PROJECT_TAG:-master}"
PROJECT_ROOT="${PROJECT_ROOT:-/tmp/Crysbox}"

BACKUP_DIR="${BACKUP_DIR:-$HOME/.crysbox-backup}"
INSTALL_RECORD="${INSTALL_RECORD:-$HOME/.crysbox-installed}"

DIR_BOX="Crysbox"
DIR_NEGPID="NEGPID"
DIR_EXPORT="export-symbols"

BOX_TARGET_DIR="fs"
BOX_MAKEFILE="fs/Makefile"

# ==================== 输出 ====================

log()  { printf "\033[1;32m[install]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[install]\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31m[install]\033[0m %s\n" "$*"; }
die()  { err "$*"; exit 0; }

# ==================== 读内核版本 ====================

read_kernel_version() {
	local v
	v=$(make kernelversion 2>/dev/null)
	if [ -n "$v" ]; then
		echo "$v"
		return
	fi

	local V P S
	V=$(grep -E '^VERSION[[:space:]]*=' Makefile | head -n1 | sed 's/.*=[[:space:]]*//' | tr -d ' ')
	P=$(grep -E '^PATCHLEVEL[[:space:]]*=' Makefile | head -n1 | sed 's/.*=[[:space:]]*//' | tr -d ' ')
	S=$(grep -E '^SUBLEVEL[[:space:]]*=' Makefile | head -n1 | sed 's/.*=[[:space:]]*//' | tr -d ' ')
	echo "$V.$P.$S"
}

# ==================== 拉项目 ====================

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
		git clone "$PROJECT_REPO" "$PROJECT_ROOT" || die "克隆失败: $PROJECT_REPO"
	fi

	[ -d "$PROJECT_ROOT" ] || die "项目目录不存在: $PROJECT_ROOT"
	log "项目源码就绪: $PROJECT_ROOT"
}

# ==================== 支持列表 ====================

list_supported_kernels() {
	{
		find "$PROJECT_ROOT/$DIR_BOX"   -mindepth 2 -maxdepth 2 -type d -printf '%f\n' 2>/dev/null
		find "$PROJECT_ROOT/$DIR_NEGPID" -mindepth 2 -maxdepth 2 -type d -printf '%f\n' 2>/dev/null
	} | sort -uV
}

# ==================== 安装记录 ====================

list_installed_for_kernel() {
	local kver="$1"
	[ -f "$INSTALL_RECORD" ] || return
	awk -v k="$kver" '$3 == k {print $1, $2, $3}' "$INSTALL_RECORD"
}

record_install() {
	local feat="$1" tool="$2" kver="$3"
	mkdir -p "$(dirname "$INSTALL_RECORD")"
	echo "$feat $tool $kver" >> "$INSTALL_RECORD"
}

# ==================== 选择器 ====================

PICK_RESULT=""
pick_menu() {
	local prompt="$1"
	shift
	local arr=("$@")

	[ ${#arr[@]} -eq 0 ] && return 4

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

# ==================== 符号检测 ====================

declare -A FEATURE_SYMS

FEATURE_SYMS[Crysbox]="umount_tree evict_inodes kthread_create_on_node \
kthread_should_stop wake_up_process prepare_creds commit_creds \
kern_path kern_path_create vfs_create vfs_mknod sync_filesystem \
kallsyms_lookup_name register_kprobe unregister_kprobe"

FEATURE_SYMS[NEGPID]="change_pid pid_task put_pid task_active_pid_ns \
find_vpid pid_nr_ns"

is_global_type() {
	case "$1" in
	T|D|R|B|W) return 0 ;;
	*)         return 1 ;;
	esac
}

is_exported() {
	local sym="$1"
	[ -f "Module.symvers" ] || return 1
	awk -v s="$sym" '$2 == s {found=1; exit} END {exit !found}' Module.symvers
}

# 返回值:
#   0 = 全部 OK 或允许 (GLOBAL/NOEXPORT, STATIC/EXPORT)
#   1 = 有 STATIC/NOEXPORT 或 MISSING (拒绝)
# 输出: 打印每个符号的状态, 设置全局 HAS_BAD, BAD_LIST
HAS_BAD=0
BAD_LIST=""

check_symbols() {
	local syms="$1"
	HAS_BAD=0
	BAD_LIST=""

	for s in $syms; do
		local t
		t=$(grep -E " (T|t|D|d|R|r|B|b|W|w) $s\$" System.map 2>/dev/null | head -n1)

		if [ -z "$t" ]; then
			printf "  \033[1;31m[MISSING]\033[0m            %-24s\n" "$s"
			HAS_BAD=1
			BAD_LIST="$BAD_LIST $s"
			continue
		fi

		local type
		type=$(echo "$t" | awk '{print $2}')

		local exported=0
		is_exported "$s" && exported=1

		if is_global_type "$type"; then
			if [ "$exported" = "1" ]; then
				printf "  \033[1;32m[OK]\033[0m                  %-24s %s\n" "$s" "$type"
			else
				printf "  \033[1;33m[GLOBAL/NOEXPORT]\033[0m    %-24s %s\n" "$s" "$type"
			fi
		else
			if [ "$exported" = "1" ]; then
				printf "  \033[1;33m[STATIC/EXPORT]\033[0m      %-24s %s\n" "$s" "$type"
			else
				printf "  \033[1;31m[STATIC/NOEXPORT]\033[0m    %-24s %s\n" "$s" "$type"
				HAS_BAD=1
				BAD_LIST="$BAD_LIST $s"
			fi
		fi
	done

	[ "$HAS_BAD" = "0" ] && return 0
	return 1
}

# ==================== 备份 ====================

backup_file() {
	local f="$1"
	[ -f "$f" ] || return 0
	local dst="$BACKUP_DIR/$KVER/$f"
	mkdir -p "$(dirname "$dst")"
	cp -v "$f" "$dst"
}

# ==================== 主流程 ====================

main() {
	KERNEL_DIR="$(pwd)"

	echo "========================================"
	echo "  crysbox / negpid 安装"
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

	# 3. 判断 make 过
	if [ ! -f "vmlinux" ] || [ ! -f "System.map" ]; then
		err "内核未构建 (缺 vmlinux / System.map)"
		err "请先 make -j\$(nproc) 再跑本脚本"
		exit 0
	fi
	log "内核已构建: $KVER"

	# 4. 拉项目
	fetch_project
	echo ""

	# 5. 判断受支持
	local SUPPORTED_KERNELS
	SUPPORTED_KERNELS=$(list_supported_kernels)
	SUPPORTED=0
	for kv in $SUPPORTED_KERNELS; do
		if [ "$kv" = "$KVER" ]; then
			SUPPORTED=1
			break
		fi
	done

	echo "========================================"
	if [ "$SUPPORTED" = "1" ]; then
		printf " 当前版本: %s  | \033[1;32m受支持\033[0m\n" "$KVER"
	else
		printf " 当前版本: %s  | \033[1;31m不受支持\033[0m\n" "$KVER"
	fi
	echo " 项目目录: $PROJECT_ROOT"
	if [ -n "$SUPPORTED_KERNELS" ]; then
		echo " 支持的版本:"
		for v in $SUPPORTED_KERNELS; do
			if [ "$v" = "$KVER" ]; then
				printf "   - %s  (当前)\n" "$v"
			else
				printf "   - %s\n" "$v"
			fi
		done
	else
		echo " 支持的版本: (无)"
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

	# 6. 判断已安装
	local installed
	installed=$(list_installed_for_kernel "$KVER")
	if [ -n "$installed" ]; then
		warn "检测到当前内核 $KVER 已安装过:"
		echo "$installed" | while read -r f t k; do
			printf "   - %s %s %s\n" "$f" "$t" "$k"
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
			0)  FEATURE="$PICK_RESULT"
			    FEATURE_DIR="$PROJECT_ROOT/$FEATURE"
			    STATE="toolver"
			    ;;
			2|3) log "已取消"; exit 0 ;;
			*)  ;;
			esac
			;;

		toolver)
			mapfile -t TOOL_VERSIONS < <(list_subdirs "$FEATURE_DIR")
			pick_menu "请选择工具版本:" "${TOOL_VERSIONS[@]}"
			RET=$?
			case $RET in
			0)  TOOLVER="$PICK_RESULT"; STATE="kver" ;;
			2)  STATE="feature" ;;
			3)  log "已取消"; exit 0 ;;
			*)  ;;
			esac
			;;

		kver)
			mapfile -t KERNEL_VERSIONS < <(list_subdirs "$FEATURE_DIR/$TOOLVER")
			pick_menu "请选择内核版本:" "${KERNEL_VERSIONS[@]}"
			RET=$?
			case $RET in
			0)  TARGET_KVER="$PICK_RESULT"; STATE="syms" ;;
			2)  STATE="toolver" ;;
			3)  log "已取消"; exit 0 ;;
			*)  ;;
			esac
			;;

		syms)
			echo ""
			echo "========================================"
			printf " 符号检测: %s\n" "$FEATURE"
			echo "========================================"

			local syms="${FEATURE_SYMS[$FEATURE]}"
			check_symbols "$syms"

			if [ "$HAS_BAD" = "0" ]; then
				printf "\n \033[1;32m符号检测通过\033[0m\n"
				echo ""
				echo "  1) 继续"
				echo "  0) 返回上一层"
				echo "  q) 退出"
				echo -n "请选择 [1/0/q]: "
				read -r sel
				case "$sel" in
				1)  STATE="summary" ;;
				0)  STATE="kver" ;;
				q|Q) log "已取消"; exit 0 ;;
				*)  ;;
				esac
			else
				printf "\n \033[1;31m符号检测未通过\033[0m\n"
				echo ""
				warn "以下符号是 static 且未导出，或缺失:"
				for s in $BAD_LIST; do
					printf "  - %s\n" "$s"
				done
				echo ""
				warn "请到 $PROJECT_ROOT/$DIR_EXPORT/$KVER/ 手动应用 patch:"
				shopt -s nullglob
				local epatches=("$PROJECT_ROOT/$DIR_EXPORT/$KVER"/*.patch)
				shopt -u nullglob
				if [ ${#epatches[@]} -gt 0 ]; then
					for p in "${epatches[@]}"; do
						printf "  patch -p1 < %s\n" "$p"
					done
				else
					printf "  (目录下无 patch，请自行处理)\n"
				fi
				echo ""
				printf "  打完后 make -j\$(nproc) 重新编译，再跑本脚本\n"
				echo ""
				echo "  0) 返回上一层"
				echo "  q) 退出"
				echo -n "请选择 [0/q]: "
				read -r sel
				case "$sel" in
				0)  STATE="kver" ;;
				q|Q) log "已取消"; exit 0 ;;
				*)  ;;
				esac
			fi
			;;

		summary)
			echo ""
			echo "========================================"
			printf " 功能:     %s\n" "$FEATURE"
			printf " 工具版本: %s\n" "$TOOLVER"
			printf " 目标内核: %s\n" "$TARGET_KVER"
			printf " 当前内核: %s\n" "$KVER"
			printf " 源目录:   %s/%s/%s\n" "$FEATURE_DIR" "$TOOLVER" "$TARGET_KVER"
			printf " 符号检测: \033[1;32m通过\033[0m\n"
			echo "========================================"
			echo ""
			echo "  1) 继续"
			echo "  0) 返回上一层"
			echo "  q) 退出"
			echo -n "请选择 [1/0/q]: "
			read -r sel
			case "$sel" in
			1)  STATE="proceed" ;;
			0)  STATE="syms" ;;
			q|Q) log "已取消"; exit 0 ;;
			*)  ;;
			esac
			;;

		# ==================== 执行 ====================
		proceed)
			local SRC_DIR="$FEATURE_DIR/$TOOLVER/$TARGET_KVER"
			log "源目录: $SRC_DIR"
			echo ""

			case "$FEATURE" in
			Crysbox)
				shopt -s nullglob
				local cfiles=("$SRC_DIR"/*.c)
				shopt -u nullglob

				if [ ${#cfiles[@]} -eq 0 ]; then
					err "源目录没有 .c 文件"
					STATE="done"
					continue
				fi

				local changed_makefile=0

				for c in "${cfiles[@]}"; do
					local base
					base=$(basename "$c")
					local dst="$BOX_TARGET_DIR/$base"

					backup_file "$dst"
					cp -v "$c" "$dst"
					log "拷贝 $base -> $dst"

					local obj
					obj="$(basename "$c" .c).o"
					if grep -qE "^obj-y[[:space:]]*\+=[[:space:]].*\\b$obj\\b" "$BOX_MAKEFILE"; then
						log "$BOX_MAKEFILE 已含 $obj，跳过"
					else
						if [ "$changed_makefile" = "0" ]; then
							backup_file "$BOX_MAKEFILE"
							changed_makefile=1
						fi
						echo "obj-y += $obj" >> "$BOX_MAKEFILE"
						log "追加 obj-y += $obj 到 $BOX_MAKEFILE"
					fi
				done
				;;

			NEGPID)
				shopt -s nullglob
				local patches=("$SRC_DIR"/*.patch)
				shopt -u nullglob

				if [ ${#patches[@]} -eq 0 ]; then
					err "源目录没有 .patch 文件"
					STATE="done"
					continue
				fi

				for p in "${patches[@]}"; do
					log "应用 patch: $(basename "$p")"

					grep -E '^\+\+\+ ' "$p" | sed 's|^+++ b/||;s|^+++ ||' | while read -r f; do
						backup_file "$f"
					done

					local prefix="-p1"
					if ! head -n1 "$p" | grep -q '^--- a/'; then
						prefix="-p0"
					fi

					if patch $prefix --forward --dry-run < "$p" >/dev/null 2>&1; then
						patch $prefix --forward < "$p" || {
							err "patch 失败: $p"
							STATE="done"
							continue 2
						}
						log "patch 应用成功: $(basename "$p")"
					else
						warn "patch 已打过或冲突: $(basename "$p")"
						warn "尝试 --fuzz=3 强打"
						patch $prefix --forward --fuzz=3 < "$p" || {
							err "patch 强打失败: $p"
							STATE="done"
							continue 2
						}
					fi
				done
				;;
			esac

			echo ""
			record_install "$FEATURE" "$TOOLVER" "$TARGET_KVER"
			log "已记录到 $INSTALL_RECORD"
			log "备份目录: $BACKUP_DIR/$KVER/"
			log "========== 执行完成 =========="
			log "请重新 make 验证"
			STATE="done"
			;;

		done)
			break
			;;

		quit|*)
			log "已取消"
			exit 0
			;;
		esac
	done
}

main "$@"