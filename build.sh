#!/bin/sh
# Requires a Posix shell that supports 'local'.
#
# Build Untrunc with an AV library from source.
#
# This build script takes advantage from the fact
# that we have access to the build configuration of the AV library.
#
# REVIEW:
# - Set the AV library configure options from the CLI?
#   Only the "--enable-*" and "--disable-*" options?
# - Determine the AV libraries to link untrunc with
#   from the dependencies for avformat in config.mak?
#   Note: Libav and FFmpeg use a fixed link order.
# - Determine the compiler and its compilation flags to use with untrunc
#   from AV library's config.mak?
#   Note: The AV library configuration has info for the C-compiler,
#         but older ones don't have info for a C++-compiler.
#
# License: The same as Untrunc (see the file COPYING in ponchio/untrunc).
#

# Default configuration options (can be changed manually).
LibAv="libav-12.3"
LibAvConfigureOpts="--disable-programs --disable-doc --disable-avdevice --disable-network"
UntruncExtraLibs="-lbz2"
# Verbosity: 0=None, 1=Error, 2=Warning, 3=Info, 4=Debug, >4=Internal
Verbosity=1



# Constants.
readonly ProgName="${0##*/}"
readonly UntruncWorkDir="$(pwd)"
readonly SepLine="$(printf "%$(( ${COLUMNS:-80} - 4 ))s" "" | tr ' ' '-')"



###################
# Local Functions #
###################

## usage()
# Print how to use this script and exit.
# Usage: usage <status-code>
#   - <status-code>   Returned exit code [0..127].
usage()
{
    local rc=${1:-0}
    cat <<EOF
${ProgName} - Build Untrunc.

Description:
  Build Untrunc with an AV library from source.
  (default AV library: ${LibAv})

Usage:
  ${ProgName} [options] <av-lib>[-<vers>]
    <av-lib>        The AV library to use: libav | ffmpeg .
    <vers>          Use the AV library release version <vers>
                    instead of the development sources from git.
  options:
    --help, -h      This help text.
    --verbosity=<n> Set verbosity level to <n> (default: ${Verbosity}):
                    0=None, 1=Error, 2=Warning, 3=Info, 4=Debug.
    --quiet, -q     Set verbosity level to 1 - errors only.


Examples:
  Use the default (${LibAv}) release sources:
    $ ./${ProgName}
  Use Libav development sources from its repository (HEAD from 'master'):
    $ ./${ProgName} libav
  Use FFmpeg 4.0 release sources:
    $ ./${ProgName} ffmpeg-4.0
EOF
    exit $rc
}

## log()
# Print formatted message to stderr if abs(verbosity-level) <= Verbosity.
#   Prepends "<verbosity-level-string>: " if verbosity-level > 0.
#   Prepends "${ProgName}: " (always).
# Usage: log <verbosity-level> "message-format" ["arguments"...]
#   - <verbosity-level>   Verbosity level of the message to print [-5..5].
#   - "message-format"    A printf format string.
#   - "arguments"         Arguments for the printf format string.
log()
{
    local lvl=$(( ${1} ))
    local fmt="${2}"
    shift 2
    if [ ${lvl#-} -le $Verbosity ]; then
        lvlStr=""
        if [ $lvl -gt 0 ]; then
            case ${lvl} in
                1)  lvlStr="Error: ";;
                2)  lvlStr="Warning: ";;
                3)  lvlStr="Info: ";;
                4)  lvlStr="Debug: ";;
                *)  lvlStr="Internal+${lvl}: ";;
            esac
        fi
        printf "${ProgName}: ${lvlStr}${fmt}\n" "$@" 1>&2
    fi
}

## die()
# Print formatted message to stderr if 1 <= Verbosity (see: log()), and exit.
# Usage: <some-command> || die [status-code] "message-format" ["arguments"...]
#   - [status-code]     Returned exit code [0..127] or $? if no status-code given.
#   - "message-format"  A printf format string.
#   - "arguments"       Arguments for the printf format string.
die()
{
    local rc=$?
    case "${1}" in
        *[!0-9]* | '')  ;;
        *)              rc="$1"; shift;;
    esac
    log 1 "$@"
    # Show a bit of help when dying due to an invalid argument.
    [ $rc -eq 2 ] && log -1 "Please see: ./${ProgName} --help"
    #[ $rc -eq 2 ] && { printf "\n"; usage $rc; }
    exit $rc
}

## isNumber()
# Is the argument an arithmetic expression that evaluates to a valid number?
# Usage: if isnum "expression"; then...
# Note:
# - zsh: set -u doesn't work for vars without a leading '$' in arithmetic evaluations.
isNumber()
{
    ( set -efuC; dummy=$(( ${1} )) ) 2> /dev/null
}



#################
# Parse Options #
#################
optionVerbosity=${Verbosity}
for option; do
    optionValue="${option#*=}"
    case "${option}" in
        --help | -h)        usage 0;;
        --verbosity=*)      optionVerbosity="-1"
                            isNumber "${optionValue}" && optionVerbosity=$(( ${optionValue} ))
                            [ $optionVerbosity -ge 0 ] || { log 1 "Invalid argument for option '%s'.\n" "${option}"; usage 2; };;
        --quiet | -q)       optionVerbosity=1
                            if case " ${LibAvConfigureOpts} " in (*--quiet*) false;; (*) true;; esac; then
                                LibAvConfigureOpts="--quiet ${LibAvConfigureOpts}"
                            fi;;
        libav  | libav-*)   LibAv="${option}";;
        ffmpeg | ffmpeg-*)  LibAv="${option}";;
        *)                  log 1 "Unknown option '%s'.\n" "${option}"; usage 2;;
    esac
done
Verbosity=${optionVerbosity}
log 4 "Verbosity          = '%s'." "${Verbosity}"
log 4 "LibAv              = '%s'." "${LibAv}"
log 4 "LibAvConfigureOpts = '%s'." "${LibAvConfigureOpts}"



####################
# Build AV Library #
####################
cd .. || die "Cannot leave Untrunc source directory '%s'." "${UntruncWorkDir}/.."
ret=0


# Get the AV library.
if case "${LibAv}" in (*-*) true;; (*) false;; esac; then
    # Use release sources from a release tar-ball.
    wget -N https://${LibAv%%-*}.org/releases/${LibAv}.tar.xz && tar -xJf ${LibAv}.tar.xz
    ret=$?
elif [ -d "./${LibAv}/.git" ]; then
    # Update existing repository.
    cd -- "${LibAv}" && git pull --rebase=preserve --prune
    ret=$?
else
    # Use development sources from a git repository.
    case "${LibAv}" in
        ffmpeg) # git release tags: n3.4, n3.4.1, n3.4.2, n4.0, n4.0.1, n4.1, etc.
            git clone --branch master --depth 3 --shallow-submodules https://github.com/FFmpeg/FFmpeg.git ffmpeg
            #git clone --branch master --depth 3 --shallow-submodules https://git.ffmpeg.org/ffmpeg.git ffmpeg
            ;;
        libav)  # git release tags: v12, v12.1, v12.2, v12.3, etc.
            git clone --branch master --depth 3 --shallow-submodules https://github.com/libav/libav.git
            #git clone --branch master --depth 3 --shallow-submodules https://git.libav.org/libav.git
            ;;
        *)  # should never happen.
            false
            ;;
    esac
    ret=$?
    # Update existing repository if it was already cloned.
    [ $ret -ne 0 ] && cd -- "${LibAv}" && git pull --rebase=preserve --prune
fi


# Check that the AV library is reachable from Untrunc.
libAvDir="../${LibAv}"
cd -- "${UntruncWorkDir}"
[ -d "${libAvDir}" ] || { [ $ret -eq 0 ] && ret=1; die $ret "No source library '%s'." "${LibAv}"; }
cd -- "${libAvDir}" || die "No source library '%s'." "${LibAv}"


# Use an external assembler?
if { ! type nasm > /dev/null 2>&1; } && { ! type yasm > /dev/null 2>&1; }; then
    if   grep -qs "disable-x86asm" ./configure 2> /dev/null 1>&2; then
        LibAvConfigureOpts="${LibAvConfigureOpts} --disable-x86asm"
    elif grep -qs "disable-yasm"   ./configure 2> /dev/null 1>&2; then
        LibAvConfigureOpts="${LibAvConfigureOpts} --disable-yasm"
    else
        die 1 "Could not disable external assembler for library '%s'." "${LibAv}"
    fi
fi


# Build the AV library.
optionsStr=""
for option in ${LibAvConfigureOpts}; do
    optionsStr="${optionsStr} ${option#--}"
done
printf "\n\nBuilding '%s' (config:%s).\n" "${LibAv}" "${optionsStr}"
printf "%s\n" "${SepLine}"
ret=0
#[ $ret -eq 0 ] && { make clean; printf "%s\n" "${SepLine}"; }
[ $ret -eq 0 ] && { ./configure ${LibAvConfigureOpts}; ret=$?; }
[ $ret -eq 0 ] && printf "%s\n" "${SepLine}"
[ $ret -eq 0 ] && { nice make -j 8 -l 50; ret=$?; }
printf "%s\n" "${SepLine}"
[ $ret -eq 0 ] || { cd -- "${UntruncWorkDir}"; die $ret "Failed to build library '%s'." "${LibAv}"; }



#################
# Build Untrunc #
#################
cd -- "${UntruncWorkDir}" || die "No Untrunc source directory '%s'." "${UntruncWorkDir}"


# Select resample library.
libAvResample=""
# The AV library may contain both swresample (prefered) and avresample.
for libAvResample in "swresample" "avresample" ""; do
    [ -d "${libAvDir}/lib${libAvResample}" ] && break
done
[ -n "${libAvResample}" ] || die 1 "Unknown resample library for '%s'." "${LibAv}"


# Add extra libraries to link with the executable.
libAvCfgBuildDir=""
for libAvCfgBuildDir in "ffbuild" "avbuild" ""; do
    [ -d "${libAvDir}/${libAvCfgBuildDir}" ] && break
done
[ -n "${libAvCfgBuildDir}" ] && libAvCfgBuildDir="${libAvCfgBuildDir}/"
libAvExtraLibs=""
if   [ -f "${libAvDir}/${libAvCfgBuildDir}config.sh"  ]; then
    # Get extra libraries from config shell script.
    . ${libAvDir}/${libAvCfgBuildDir}config.sh
    libAvExtraLibs="${extralibs_avutil} ${extralibs_avcodec} ${extralibs_avformat} ${extralibs_avresample} ${extralibs_swresample}"
elif [ -f "${libAvDir}/${libAvCfgBuildDir}config.mak" ]; then
    # Get extra libraries from config make script.
    for libAvNm in "avutil" "avcodec" "avformat" "avresample" "swresample" ""; do
        elibs="$(grep "^[[:blank:]]*EXTRALIBS${libAvNm:+"-${libAvNm}"}[[:blank:]]*=" "${libAvDir}/${libAvCfgBuildDir}config.mak")"
        elibs="${elibs#*=}"
        log 4 "libAvNm = '%s' -> 'EXTRALIBS%s' -> '%s'." "${libAvNm}" "${libAvNm:+"-${libAvNm}"}" "${elibs}"
        libAvExtraLibs="${libAvExtraLibs} ${elibs}"
    done
fi
if [ -n "${libAvExtraLibs}" ]; then
    # Use the extra libraries from the AV library.
    UntruncExtraLibs=""
else
    # Use the default extra libraries.
    # These defaults are for older AV libraries.
    # Newer AV libraries are expected to supply a config.sh file.
    log 2 "Warning: Guessing the extra libraries to link with."
    libAvExtraLibs="-pthread -lm -lz"
    if case "${OS_TYPE:-"$(uname)"}" in ([Dd]arwin*) true;; (*) false;; esac; then
        # macOS libraries for hardware acceleration (VideoDecodeAcceleration has been superseded by VideoToolBox).
        libAvExtraLibs="${libAvExtraLibs} -framework CoreFoundation -framework CoreVideo -framework VideoDecodeAcceleration"
        # macOS libraries for avdevice when AVFoundation/AVFoundation.h exists.
        #libAvExtraLibs="${libAvExtraLibs} -framework Foundation -framework AVFoundation -framework CoreVideo -framework CoreMedia"
    fi
fi
optionPrefix=""
for option in ${libAvExtraLibs}; do
    [ "${option}" = "-framework" ] && { optionPrefix="${option} "; continue; }
    if case " ${UntruncExtraLibs} " in (*" ${option} "*) false;; (*) true;; esac; then
        UntruncExtraLibs="${UntruncExtraLibs} ${optionPrefix}${option}"
    fi
    optionPrefix=""
done
log 4 "libAvExtraLibs     = '%s'." "${libAvExtraLibs}"
log 4 "UntruncExtraLibs   = '%s'." "${UntruncExtraLibs}"


# Build Untrunc program.
optionsStr=""
for option in ${UntruncExtraLibs}; do
    optionsStr="${optionsStr} ${option#-l}"
done
printf "\n\nBuilding Untrunc (extra-libs:%s).\n" "${optionsStr}"
printf "%s\n" "${SepLine}"
[ $ret -eq 0 ] && rm ./untrunc 2> /dev/null
[ $ret -eq 0 ] && { g++ -o untrunc -I${libAvDir} file.cpp main.cpp track.cpp atom.cpp mp4.cpp -L${libAvDir}/libavformat -lavformat -L${libAvDir}/libavcodec -lavcodec -L${libAvDir}/lib${libAvResample} -l${libAvResample} -L${libAvDir}/libavutil -lavutil ${UntruncExtraLibs}; ret=$?; }
#[ $ret -eq 0 ] && { g++ -std=c++98 -o untrunc -I${libAvDir} file.cpp main.cpp track.cpp atom.cpp mp4.cpp -L${libAvDir}/libavformat -lavformat -L${libAvDir}/libavcodec -lavcodec -L${libAvDir}/lib${libAvResample} -l${libAvResample} -L${libAvDir}/libavutil -lavutil ${UntruncExtraLibs}; ret=$?; }
printf "%s\n" "${SepLine}"
[ $ret -eq 0 ] || die $ret "Failed to build Untrunc with library '%s'." "${LibAv}"

# Fin.
exit $ret
