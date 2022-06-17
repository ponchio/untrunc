#!/bin/sh

# check for git short hash
revision=$(cd "$1" && git describe --always 2> /dev/null)

# no revision number found
test "$revision" || revision=$(cd "$1" && cat RELEASE 2> /dev/null)

# releases extract the version number from the VERSION file
version=$(cd "$1" && cat VERSION 2> /dev/null)
test "$version" || version=$revision

test -n "$3" && version=$version-$3

if [ -z "$2" ]; then
    echo "$version"
    exit
fi

NEW_REVISION="#define LIBAV_VERSION \"$version\""
OLD_REVISION=$(cat "$2" 2> /dev/null)

# Update version.h only on revision changes to avoid spurious rebuilds
if test "$NEW_REVISION" != "$OLD_REVISION"; then
    echo "$NEW_REVISION" > "$2"
fi
