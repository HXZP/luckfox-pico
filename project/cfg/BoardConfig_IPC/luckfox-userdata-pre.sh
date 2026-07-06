#!/bin/bash

function lf_rm() {
    for file in "$@"; do
        if [ -e "$file" ]; then
            echo "Deleting: $file"
            rm -rf "$file"  
        #else
            #echo "File not found: $file" 
        fi
    done
}

# remove unused files
function remove_data()
{
    lf_rm $RK_PROJECT_PACKAGE_USERDATA_DIR/*.sh
    lf_rm $RK_PROJECT_PACKAGE_USERDATA_DIR/*.bmp
}

function copy_userdata_overlay()
{
    local board_dir
    local overlay_dir
    local overlay_userdata_dir

    if [ -n "$BOARD_CONFIG" ] && [ -e "$BOARD_CONFIG" ]; then
        board_dir="$(dirname "$(realpath "$BOARD_CONFIG")")"
    else
        board_dir="$(cd "$(dirname "$0")" && pwd)"
    fi
    mkdir -p "$RK_PROJECT_PACKAGE_USERDATA_DIR"

    for overlay_dir in $RK_POST_OVERLAY; do
        overlay_userdata_dir="$board_dir/overlay/$overlay_dir/userdata"
        if [ -d "$overlay_userdata_dir" ]; then
            echo "Applying userdata overlay: $overlay_userdata_dir"
            cp -rfa "$overlay_userdata_dir"/* "$RK_PROJECT_PACKAGE_USERDATA_DIR"/
        fi
    done
}

#=========================
# run
#=========================
remove_data
copy_userdata_overlay
