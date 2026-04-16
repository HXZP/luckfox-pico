#!/bin/sh

case $1 in
        start)
        killall rkipc
                ;;
        stop)
                ;;
        *)
                exit 1
                ;;
esac
