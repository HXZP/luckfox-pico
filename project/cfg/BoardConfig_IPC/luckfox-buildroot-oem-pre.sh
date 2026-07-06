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
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/*.aiisp
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/*.data
    
    # drm ( sample program required )
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libdrm*
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libdrm_rockchip*

    # kms
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libkms*

    # freetype
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libfreetype*

    # iconv
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libiconv*

    # rkAVS
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/librkAVS*
    
    # jpeg
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libjpeg*

    # png
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/lib/libpng*

    # vqefiles
    lf_rm $RK_PROJECT_PACKAGE_OEM_DIR/usr/share/vqefiles/*
}

function disable_auto_speaker_test()
{
    local rk_lunch="$RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/RkLunch.sh"

    if [ ! -f "$rk_lunch" ]; then
        echo "Skip speaker-test patch: $rk_lunch not found"
        return
    fi

    if grep -q '/userdata/enable_speaker_test' "$rk_lunch"; then
        return
    fi

    sed -i \
        's#\[ -f "/oem/usr/share/speaker_test.wav" \]#[ -f "/userdata/enable_speaker_test" ] \&\& [ -f "/oem/usr/share/speaker_test.wav" ]#' \
        "$rk_lunch"
}

#=========================
# run
#=========================
remove_data
disable_auto_speaker_test
