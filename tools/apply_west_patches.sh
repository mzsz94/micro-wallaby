#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if command -v west >/dev/null 2>&1; then
	west_command=west
elif [ -x "$repository_root/.venv/bin/west" ]; then
	west_command="$repository_root/.venv/bin/west"
else
	echo "west was not found; activate the Zephyr environment first" >&2
	exit 1
fi

cd "$repository_root"

hal_realtek=$("$west_command" list -f '{abspath}' hal_realtek)
zephyr=$("$west_command" list -f '{abspath}' zephyr)

apply_patch_directory()
{
	target_repository=$1
	patch_directory=$2
	target_name=$3

	for current_patch in "$patch_directory"/*.patch; do
		[ -f "$current_patch" ] || continue
		patch_name=$(basename "$current_patch")
		if git -C "$target_repository" apply --reverse --check "$current_patch" 2>/dev/null; then
			echo "$target_name patch already applied: $patch_name"
			continue
		fi

		git -C "$target_repository" apply --check "$current_patch"
		git -C "$target_repository" apply "$current_patch"
		echo "Applied $target_name patch: $patch_name"
	done
}

apply_patch_directory "$hal_realtek" "$repository_root/patches/hal_realtek" hal_realtek
apply_patch_directory "$zephyr" "$repository_root/patches/zephyr" zephyr
