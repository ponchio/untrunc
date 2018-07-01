# Docker Image for Untrunc.
# Uses a Debian-based parent image.

##################################
# Docker Build Environment Image #
##################################
# The LABEL, ENV and ARG are inherited from the Docker 'ubuntu' image.
FROM ubuntu AS build-env
#RUN printf "================\n"; printf "Docker build-environment image\n"; pwd; env | sort; printf "================\n"
# Persistent non-interactive image.
ENV DEBIAN_FRONTEND=noninteractive
# Terminal used during Docker image build.
ARG TERM=linux

# OCI annotation of Untrunc.
LABEL org.opencontainers.title="Untrunc"
LABEL org.opencontainers.description="Restore a damaged (truncated) mp4, m4v, mov, 3gp video. Provided you have a similar not broken video. And some luck."
LABEL org.opencontainers.source="https://github.com/ponchio/untrunc"
LABEL org.opencontainers.licenses="GPL-2.0-or-later"
#LABEL org.opencontainers.image.documentation="<URL to get documentation on the image (how-to-use)>"
LABEL org.opencontainers.image.authors="Federico Ponchio <ponchio@gmail.com>"
#LABEL org.opencontainers.image.authors="https://github.com/ponchio/untrunc/graphs/contributors"
# Following OCI annotations are set from outside this Dockerfile,
# - so external commands can be used to retrieve the label information and
# - so no label will be set if there is no label information.
# label org.opencontainers.image.version="$(git describe --abbrev=8 2> /dev/null || git describe --tags --abbrev=8 2> /dev/null)"
# label org.opencontainers.image.revision="${TRAVIS_COMMIT:-"$(git log -n 1 --format='format:%H' 2> /dev/null)"}"
# label org.opencontainers.image.created="$(date -u '+%Y-%m-%dT%TZ')"



# LIBAV: Configure AV library to use.
# (can be overruled with: --build-arg "LIBAV=*")
#   - dev             OS development packages,
#   - dev-headers     OS development packages with internal header files,
#   - libav-<vers>    Libav  version <vers> build from source (like: "libav-12.3"),
#   - ffmpeg-<vers>   FFmpeg version <vers> build from source (like: "ffmpeg-4.0.1"),
#   - libav           Libav  build from sources from git master,
#   - ffmpeg          FFmpeg build from sources from git master,
#   - ""              The default AV library = dev.
# Build results for Untrunc production image:
#   - ubuntu parent image           :  81 MiB,
#   - dev / dev-headers             : 264 MiB,  4:15 min,
#   - libav-12.3 only, no extra libs:  96 MiB,  5:15 min,
#   - libav-12.3 + 3GP,MP3,H264,H265: 108 MiB,  6:15 min,
#   - ffmpeg-4.0.1  + all extra libs: 131 MiB, 10:30 min.
#ARG LIBAV="dev"
#ARG LIBAV="dev-headers"
ARG LIBAV="libav-12.3"
#ARG LIBAV="ffmpeg-4.0.1"
#ARG LIBAV="libav"
#ARG LIBAV="ffmpeg"


# LIBAV_SRC_CONF: Configure the AV library options to use when building from source.
# (can be overruled with: --build-arg "LIBAV_SRC_CONF=*")
# Add support for: 3GP audio, MP3 audio, H.264 AVC video.
#ARG LIBAV_SRC_CONF="--enable-libgsm --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libvo-amrwbenc --enable-libmp3lame --enable-libx264"
# Add support for: 3GP audio, MP3 audio, H.264 AVC video, H.265 HEVC video.
ARG LIBAV_SRC_CONF="--enable-libgsm --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libvo-amrwbenc --enable-libmp3lame --enable-libx264 --enable-libx265"
# Add support for: 3GP audio, MP3 audio, H.264 AVC video, H.265 HEVC video, SoX Resampler.
#ARG LIBAV_SRC_CONF="--enable-libgsm --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libvo-amrwbenc --enable-libmp3lame --enable-soxr --enable-libx264 --enable-libx265"
# Add support for: 3GP audio, MP2 audio, MP3 audio, H.264 AVC video, H.265 HEVC video, SoX Resampler, WebP format, VP8/9 video, Opus audio, Theora video, Speex audio, Mod audio, MJPEG video, Wav/PCM compression.
#ARG LIBAV_SRC_CONF="--enable-libgsm --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libvo-amrwbenc --enable-libtwolame --enable-libmp3lame --enable-soxr --enable-libx264 --enable-libx265 --enable-libwebp --enable-libvpx --enable-libopus --enable-libtheora --enable-libspeex --enable-libopenmpt --enable-libopenjpeg --enable-libwavpack"
#
# --External-Library          L [License] Description [default] (comment)
# Audio Codecs:
# --enable-libgsm             a [GPLv3] enable GSM de/encoding via libgsm [no]
# --enable-libopencore-amrnb  a [GPLv3] enable AMR-NB de/encoding via libopencore-amrnb [no] (Adaptive Multi-Rate Narrowband (a.k.a. GSM-AMR), used in 3gp Container)
# --enable-libopencore-amrwb  a [GPLv3] enable AMR-WB decoding via libopencore-amrwb [no]    (Adaptive Multi-Rate Wideband, ITU-T G.722.2)
# --enable-libvo-amrwbenc     a [GPLv3] enable AMR-WB encoding via libvo-amrwbenc [no]
# --enable-libopus            a enable Opus de/encoding via libopus [no] (replaces Speex and Vorbis)
# --enable-libspeex           a enable Speex de/encoding via libspeex [no] (obsolete -> use Opus)
# --enable-libvorbis          a enable Vorbis en/decoding via libvorbis, native implementation exists [no] (obsolete -> use Opus)
# --enable-libmp3lame         a enable MP3 encoding via libmp3lame [no] (MPEG-3 audio)
# --enable-libtwolame         a enable MP2 encoding via libtwolame [no] (MPEG-2 audio)
# --enable-libopenmpt         s enable decoding tracked files via libopenmpt [no] (Mod audio)
# Image Codecs:
# --enable-libopenjpeg        s enable JPEG 2000 de/encoding via OpenJPEG [no] (MJPEG video)
# Video Codecs:
# --enable-libtheora          a enable Theora encoding via libtheora [no] (VP3-based, obsolete -> use VP9+ from libvpx)
# --enable-libvpx             a enable VP8 and VP9 de/encoding via libvpx [no]
# --enable-libx264            a [GPLv2] enable H.264 encoding via x264 [no] (H.264 AVC  (MPEG-4 Part 10) video)
# --enable-libx265            s [GPLv2] enable HEVC encoding via x265 [no]  (H.265 HEVC (MPEG-H Part  2) video)
# Resamplers:
# --enable-libsoxr            s enable Include libsoxr resampling [no] (audio resampling) (used in FFmpeg libswresample, pulls in libmpg123, libogg & libvorbis libraries)
# Compression Codecs:
# --enable-libwavpack         a enable wavpack encoding via libwavpack [no] (wav & pcm audio compression [.wv])
# AV Containers:
# --enable-libwebp            a enable WebP encoding via libwebp [no] (you should enable libvpx and libopus as well)
#
# Legenda:
# - L = available Library: a = both static Archive & dynamic shared library, s = dynamic Shared library only.
# Note:
# - libx265 is only usable as a shared library due to a bug in its static archive.
#   (XXX BUG: 'libx265.a' cannot be statically linked XXX https://bugs.launchpad.net/ubuntu/+source/x265/+bug/1777875 XXX)


# LIBAV_SRC_SITE: Configure the site to retrieve the AV library sources from.
# (can be overruled with: --build-arg "LIBAV_SRC_SITE=*")
# - origin      The original site of the AV library's project,
# - github      The mirror on GitHub of the AV library's project,
# - ""          The default site.
#ARG LIBAV_SRC_SITE="origin"
#ARG LIBAV_SRC_SITE="github"


# Make AV library settings persistent.
ENV LIBAV="${LIBAV:-dev}"
ENV LIBAV_SRC_CONF="${LIBAV_SRC_CONF}"

# Build directory.
WORKDIR /usr/src
# Location of the AV library sources.
ENV LIBAV_DIR="/usr/src/${LIBAV:-dev}"

# Show image settings.
#RUN printf "LIBAV          = '%s'\n" "${LIBAV}"; \
#    printf "LIBAV_DIR      = '%s'\n" "${LIBAV_DIR}"; \
#    if [ "${LIBAV%%[-+]*}" != "dev" ]; then \
#        printf "LIBAV_SRC_CONF = '%s'\n" "${LIBAV_SRC_CONF}"; \
#        printf "LIBAV_SRC_SITE = '%s'\n" "${LIBAV_SRC_SITE}"; \
#    fi



# Install packages.
# Build Untrunc with an AV library with the maximum number of capabilities,
#   because the result is used as a stand-alone Docker-executable "docker-untrunc".
# When using the AV library headers from the source package ("dev-headers"):
#   - till  Ubuntu 10.10 (Maverick Meerkat): libav* = FFmpeg (ffmpeg source package),
#   - since Ubuntu 11.04 (Natty Narwhal)   : libav* = Libav  (libav  source package),
#   - till  Ubuntu 14.10 (Utopic Unicorn)  : libav* = Libav  (libav  source package),
#   - since Ubuntu 15.04 (Vivid Vervet)    : libav* = FFmpeg (ffmpeg source package).
#   For Ubuntu Long-Time-Support (LTS) versions:
#   - till  Ubuntu 10.04 LTS (Lucid Lynx)      : libav* = FFmpeg (ffmpeg source package),
#   - since Ubuntu 12.04 LTS (Precise Pangolin): libav* = Libav  (libav  source package),
#   - till  Ubuntu 14.04 LTS (Trusty Tahr)     : libav* = Libav  (libav  source package),
#   - since Ubuntu 16.04 LTS (Xenial Xerus)    : libav* = FFmpeg (ffmpeg source package).
# No need to run 'apt-get clean' as it will run automatically after 'apt-get install'.
RUN printf "\n%$(( ${COLUMNS:-80} - 4 ))s\n" "" | tr ' ' '-'; \
    printf "LIBAV          = '%s'\n" "${LIBAV}"; \
    if   [ "${LIBAV:-dev}" = "dev" ]; then \
        LIBAV_PKGS="libavformat libavcodec libavutil"; \
        LIBAV_PKGS_DEV=""; \
        for option in ${LIBAV_PKGS} libnuma; do \
            option="${option#dev_}"; \
            [ "${option#ins_}" = "${option}" ] || continue; \
            LIBAV_PKGS_DEV="${LIBAV_PKGS_DEV} ${option}-dev"; \
        done; \
        printf "LIBAV_PKGS_DEV = '%s'\n" "${LIBAV_PKGS_DEV}"; \
        apt-get update \
        && apt-get install -y \
            wget g++ binutils pkg-config \
            ${LIBAV_PKGS_DEV} ; \
    elif [ "${LIBAV}" = "dev-headers" ]; then \
        LIBAV_PKGS="libavformat libavcodec libavutil"; \
        LIBAV_PKGS_DEV=""; \
        for option in ${LIBAV_PKGS} libnuma; do \
            option="${option#dev_}"; \
            [ "${option#ins_}" = "${option}" ] || continue; \
            LIBAV_PKGS_DEV="${LIBAV_PKGS_DEV} ${option}-dev"; \
        done; \
        . /etc/os-release; \
        VERS_ID_MAJOR="${VERSION_ID%%.*}"; \
        LIBAV_SRC_PKG="ffmpeg"; \
        if [ $VERS_ID_MAJOR -ge 11 ] && [ $VERS_ID_MAJOR -le 14 ]; then LIBAV_SRC_PKG="libav"; fi; \
        printf "VERSION_ID     = '%s'\n" "${VERSION_ID}"; \
        printf "LIBAV_PKGS_DEV = '%s'\n" "${LIBAV_PKGS_DEV}"; \
        printf "LIBAV_SRC_PKG  = '%s'\n" "${LIBAV_SRC_PKG}"; \
        apt-get update \
        && apt-get install -y \
            wget g++ binutils pkg-config \
            dpkg \
            ${LIBAV_PKGS_DEV} \
        && apt-get build-dep -y \
            ${LIBAV_SRC_PKG} \
        && apt-get source -y \
            ${LIBAV_SRC_PKG} \
        && apt-get clean -y ; \
    elif [ "${LIBAV%%-*}" = "libav" ] || [ "${LIBAV%%-*}" = "ffmpeg" ]; then \
        LIBAV_PKGS=""; \
        for option in ${LIBAV_SRC_CONF}; do \
            case "${option}" in \
                --enable-libgsm)            LIBAV_PKGS="${LIBAV_PKGS}"' libgsm[1-9]';; \
                --enable-libopencore-amrnb) LIBAV_PKGS="${LIBAV_PKGS}"' libopencore-amrnb';; \
                --enable-libopencore-amrwb) LIBAV_PKGS="${LIBAV_PKGS}"' libopencore-amrwb';; \
                --enable-libvo-amrwbenc)    LIBAV_PKGS="${LIBAV_PKGS}"' libvo-amrwbenc';; \
                --enable-libmp3lame)        LIBAV_PKGS="${LIBAV_PKGS}"' libmp3lame';; \
                --enable-libopenjpeg)       LIBAV_PKGS="${LIBAV_PKGS}"' libopenjp2-[7-9]';; \
                --enable-libopenmpt)        LIBAV_PKGS="${LIBAV_PKGS}"' libopenmpt';; \
                --enable-libopus)           LIBAV_PKGS="${LIBAV_PKGS}"' libopus';; \
                --enable-libsnappy)         LIBAV_PKGS="${LIBAV_PKGS}"' libsnappy';; \
                --enable-libsoxr)           LIBAV_PKGS="${LIBAV_PKGS}"' libsoxr';; \
                --enable-libspeex)          LIBAV_PKGS="${LIBAV_PKGS}"' libspeex';; \
                --enable-libtheora)         LIBAV_PKGS="${LIBAV_PKGS}"' libtheora';; \
                --enable-libtwolame)        LIBAV_PKGS="${LIBAV_PKGS}"' libtwolame';; \
                --enable-libvorbis)         LIBAV_PKGS="${LIBAV_PKGS}"' libvorbis ins_libvorbisenc';; \
                --enable-libvpx)            LIBAV_PKGS="${LIBAV_PKGS}"' libvpx';; \
                --enable-libwavpack)        LIBAV_PKGS="${LIBAV_PKGS}"' libwavpack';; \
                --enable-libwebp)           LIBAV_PKGS="${LIBAV_PKGS}"' libwebp ins_libwebpmux ins_libwebpdemux';; \
                --enable-libx264)           LIBAV_PKGS="${LIBAV_PKGS}"' libx264 dev_opencl';; \
                --enable-libx265)           LIBAV_PKGS="${LIBAV_PKGS}"' libx265 libnuma';; \
            esac; \
        done; \
        LIBAV_PKGS_DEV=""; \
        for option in ${LIBAV_PKGS}; do \
            option="${option#dev_}"; \
            [ "${option#ins_}" = "${option}" ] || continue; \
            LIBAV_PKGS_DEV="${LIBAV_PKGS_DEV} ${option}-dev"; \
        done; \
        printf "LIBAV_SRC_CONF = '%s'\n" "${LIBAV_SRC_CONF}"; \
        printf "LIBAV_PKGS_DEV = '%s'\n" "${LIBAV_PKGS_DEV}"; \
        apt-get update \
        && apt-get install -y \
            wget g++ binutils pkg-config \
            gcc nasm make zlib1g-dev libbz2-dev liblzma-dev \
            ${LIBAV_PKGS_DEV} ; \
    else \
        printf "Unknown AV library '%s'.\n" "${LIBAV}" 1>&2; \
        exit 2; \
    fi \
    && { rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /usr/share/doc/* /usr/share/man/* || true; } \
    && { printf "%$(( ${COLUMNS:-80} - 4 ))s\n" "" | tr ' ' '-'; } \
    && LIBAV_PKGS_INS="" \
    && for option in ${LIBAV_PKGS}; do \
            option="${option#ins_}"; \
            if   [ "${option#dev_}"  != "${option}" ]; then \
                continue; \
            elif [ "${option%[]?*]}" != "${option}" ]; then \
                LIBAV_PKGS_INS="${LIBAV_PKGS_INS} ${option}"; \
            elif [ "${option%[0-9]}" != "${option}" ]; then \
                LIBAV_PKGS_INS="${LIBAV_PKGS_INS} ${option}"'-[0-9]'; \
            else \
                LIBAV_PKGS_INS="${LIBAV_PKGS_INS} ${option}"'[0-9]'; \
            fi; \
       done \
    && printf "LIBAV_PKGS     = '%s'\n" "${LIBAV_PKGS}" \
    && printf "LIBAV_PKGS_INS = '%s'\n" "${LIBAV_PKGS_INS}" \
    && mkdir -p /usr/local/share/untrunc \
    && { [ -z "${LIBAV_PKGS_INS}" ] || printf '%s' "${LIBAV_PKGS_INS# }" > /usr/local/share/untrunc/untrunc-pkgs.txt; } \
    && { printf "%$(( ${COLUMNS:-80} - 4 ))s\n\n" "" | tr ' ' '-'; };



# Build the AV library.
# By default the AV library is build as static Archives and is statically linked into Untrunc.
RUN printf "\n%$(( ${COLUMNS:-80} - 4 ))s\n" "" | tr ' ' '-'; \
    printf "LIBAV          = '%s'\n" "${LIBAV}"; \
    if   [ "${LIBAV}" = "dev-headers" ]; then \
        LIBAV_SUBDIR="$(find * -maxdepth 0 -type d \( -iname 'ffmpeg-*' -o -iname 'libav-*' \) )" \
        && { [ -d "${LIBAV}" ] || { ln -s "${LIBAV_SUBDIR}" "${LIBAV}"; printf "%s = '%s'\n" "${LIBAV}" "${LIBAV_SUBDIR}"; }; } \
        && cd -- "${LIBAV}" \
        && ( DEB_BUILD_PROFILES="stage1" make -f debian/rules override_dh_auto_configure || make -f debian/rules configure-stamp-shared ) \
        && for d in "debian/standard" "debian/static" "debian-shared" "debian-static"; do \
                [ -d "${d}" ] && ln -s "${d}/config.h" config.h && { printf "config.h = '%s'\n" "${d}/config.h"; break; }; \
           done; \
    elif [ "${LIBAV%%-*}" = "libav" ] || [ "${LIBAV%%-*}" = "ffmpeg" ]; then \
        case " ${LIBAV_SRC_CONF} " in \
            *[[:blank:]]--enable-version3[[:blank:]]* | *[[:blank:]]--enable-gpl3[[:blank:]]* | *[[:blank:]]--enable-gplv3[[:blank:]]*) ;; \
            *[[:blank:]]--enable-libgsm[[:blank:]]*)  LIBAV_SRC_CONF="--enable-version3 ${LIBAV_SRC_CONF}";; \
            *[[:blank:]]--enable-libopencore-amr*)    LIBAV_SRC_CONF="--enable-version3 ${LIBAV_SRC_CONF}";; \
            *[[:blank:]]--enable-libvo-amr*)          LIBAV_SRC_CONF="--enable-version3 ${LIBAV_SRC_CONF}";; \
        esac; \
        case " ${LIBAV_SRC_CONF} " in \
            *[[:blank:]]--enable-gpl[[:blank:]]* | *[[:blank:]]--enable-gpl2[[:blank:]]* | *[[:blank:]]--enable-gplv2[[:blank:]]*) ;; \
            *[[:blank:]]--enable-libx26[45][[:blank:]]*)  LIBAV_SRC_CONF="--enable-gpl ${LIBAV_SRC_CONF}";; \
        esac; \
        printf "LIBAV_SRC_SITE = '%s'\n" "${LIBAV_SRC_SITE:-github}"; \
        if [ "${LIBAV_SRC_SITE}" = "origin" ]; then \
            case "${LIBAV}" in \
                libav   | ffmpeg)   wget -O - --progress=dot:mega "https://${LIBAV%%-*}.org/releases/${LIBAV%%-*}-snapshot.tar.bz2" | tar -xj;; \
                libav-* | ffmpeg-*) wget -O - --progress=dot:mega "https://${LIBAV%%-*}.org/releases/${LIBAV}.tar.xz" | tar -xJ;; \
                *)                  false;; \
            esac; \
        else \
            case "${LIBAV}" in \
                libav)      LIBAV_REPO="libav";   LIBAV_VERS="master";; \
                libav-*)    LIBAV_REPO="libav";   LIBAV_VERS="v${LIBAV#*-}";; \
                ffmpeg)     LIBAV_REPO="FFmpeg";  LIBAV_VERS="master";; \
                ffmpeg-*)   LIBAV_REPO="FFmpeg";  LIBAV_VERS="n${LIBAV#*-}";; \
                *)          false;; \
            esac \
            && { wget -O - --progress=dot:mega "https://github.com/${LIBAV_REPO}/${LIBAV_REPO}/archive/${LIBAV_VERS}.tar.gz" | tar -xz; } \
            && { [ -d "${LIBAV}" ] || { ln -s "${LIBAV_REPO}-${LIBAV_VERS}" "${LIBAV}"; printf "%s = '%s-%s'\n" "${LIBAV}" "${LIBAV_REPO}" "${LIBAV_VERS}"; }; }; \
        fi \
        && cd -- "${LIBAV}/" \
        && LIBAV_CONFIG="" \
        && for option in ${LIBAV_SRC_CONF}; do \
                if grep -qs -e "${option}" ./configure > /dev/null; then \
                    LIBAV_CONFIG="${LIBAV_CONFIG} ${option}"; \
                fi; \
            done \
        && LIBAV_CONFIG="${LIBAV_CONFIG# }" \
        && LIBAV_ELIBS="" \
        && for option in ${LIBAV_CONFIG}; do \
                case "${option}" in \
                    --enable-libx264)   LIBAV_ELIBS="${LIBAV_ELIBS} --extra-libs=-ldl";; \
                    --enable-libx265)   LIBAV_ELIBS="${LIBAV_ELIBS} --extra-libs=-lnuma --extra-libs=-ldl";; \
                esac; \
            done \
        && LIBAV_ELIBS="${LIBAV_ELIBS# }" \
        && printf "LIBAV_SRC_CONF = '%s'\n" "${LIBAV_SRC_CONF}" \
        && printf "LIBAV_CONFIG   = '%s'\n" "${LIBAV_CONFIG}" \
        && printf "LIBAV_ELIBS    = '%s'\n" "${LIBAV_ELIBS}" \
        && ./configure --prefix=/usr/local --disable-debug --enable-gpl --enable-runtime-cpudetect --disable-programs --disable-doc --disable-avdevice --disable-network ${LIBAV_CONFIG} ${LIBAV_ELIBS} \
        && make -j 4 \
        && make install \
        && { rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /usr/share/doc/* /usr/share/man/* || true; }; \
    fi \
    && { printf "%$(( ${COLUMNS:-80} - 4 ))s\n\n" "" | tr ' ' '-'; };
# Add the newly build AV library to the pkg-config search path.
ENV PKG_CONFIG_PATH="/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+":${PKG_CONFIG_PATH}"}"




#############################
# Docker Construction Image #
#############################
# The LABEL and ENV (but not ARG) are inherited from the Docker 'build-env' image.
FROM build-env AS construction

# Get the current sources from the Docker context.
COPY . untrunc-src/

# Build Untrunc from either git master or from the current context.
# The AV library has a libavresample and/or the preferred libswresample -- pkg-config will select the right one.
# When using static AV library from source, build Untrunc statically if possible [UNTRUNC_LINK="static"].
# (XXX BUG: 'libx265.a' cannot be statically linked XXX https://bugs.launchpad.net/ubuntu/+source/x265/+bug/1777875 XXX)
#   When this bug is fixed, remove the line '*[[:blank:]]--enable-libx265[[:blank:]]*) UNTRUNC_LINK="dynamic";; \'.
# GCC run-time libraries [automatically included after all other libraries]:
#   - libgcc    internal functions  - include as shared library to handle C++ exception from shared labraries,
#   - libgcc_s  global variables    - is a shared library only,
#   - libstdc++ C++ STL.
# GCC drivers: g++ = gcc -xc++ -shared-libcc -lstdc++
RUN printf "\n%$(( ${COLUMNS:-80} - 4 ))s\n" "" | tr ' ' '-'; \
    if [ ! -f "untrunc-src/mp4.cpp" ]; then \
        rm -rf untrunc-src/; \
        { wget -O - --progress=dot:mega https://github.com/ponchio/untrunc/archive/master.tar.gz | tar -xz; } \
        && ln -s untrunc-master untrunc-src; \
    fi; \
    printf "LIBAV          = '%s'\n" "${LIBAV}"; \
    cd untrunc-src/ \
    && if   [ "${LIBAV:-dev}" = "dev" ]; then \
            g++ -o untrunc $(pkg-config --cflags libavformat libavcodec libavutil) *.cpp $(pkg-config --libs libavformat libavcodec libavutil); \
       elif [ "${LIBAV}" = "dev-headers" ]; then \
            g++ -o untrunc $(pkg-config --cflags libavformat libavcodec libavutil) -idirafter "${LIBAV_DIR}/" *.cpp $(pkg-config --libs libavformat libavcodec libavutil); \
       else \
            UNTRUNC_LINK="${UNTRUNC_LINK:-static}"; \
            UNTRUNC_PKGCFG="libavformat libavcodec libavutil"; \
            UNTRUNC_ELIBS=""; \
            case " ${LIBAV_SRC_CONF} " in \
                *[[:blank:]]--enable-libopenjpeg[[:blank:]]*)   UNTRUNC_LINK="dynamic";; \
                *[[:blank:]]--enable-libopenmpt[[:blank:]]*)    UNTRUNC_LINK="dynamic";; \
                *[[:blank:]]--enable-libsoxr[[:blank:]]*)       UNTRUNC_LINK="dynamic";; \
                *[[:blank:]]--enable-libx265[[:blank:]]*)       UNTRUNC_LINK="dynamic";; \
            esac; \
            for option in ${LIBAV_SRC_CONF}; do \
                case "${option}" in \
                    --enable-libopenmpt)    UNTRUNC_PKGCFG="${UNTRUNC_PKGCFG} libopenmpt";; \
                    --enable-libwebp)       UNTRUNC_PKGCFG="${UNTRUNC_PKGCFG} libwebpdemux libwebpmux libwebp";; \
                    --enable-libx264)       UNTRUNC_PKGCFG="${UNTRUNC_PKGCFG} x264";; \
                    --enable-libx265)       UNTRUNC_ELIBS="${UNTRUNC_ELIBS} -lnuma $(pkg-config --libs ${UNTRUNC_LINK:+"--${UNTRUNC_LINK%-*}"} x265 | sed -e 's/-l\(gcc\|gcc_s\|stdc[+][+]\)\>//g')";; \
                esac; \
            done; \
            [ "${UNTRUNC_LINK%-*}"  = "static"  ] || UNTRUNC_ELIBS="${UNTRUNC_ELIBS} -ldl"; \
            UNTRUNC_ELIBS="${UNTRUNC_ELIBS# }"; \
            printf "LIBAV_SRC_CONF = '%s'\n" "${LIBAV_SRC_CONF}"; \
            printf "UNTRUNC_LINK   = '%s'\n" "${UNTRUNC_LINK}"; \
            printf "UNTRUNC_PKGCFG = '%s'\n" "${UNTRUNC_PKGCFG}"; \
            printf "UNTRUNC_ELIBS  = '%s'\n" "${UNTRUNC_ELIBS}"; \
            [ "${UNTRUNC_LINK%-*}" != "static"  ] || rm -f "/usr/local/share/untrunc/untrunc-pkgs.txt" || true; \
            [ "${UNTRUNC_LINK%-*}" != "dynamic" ] || UNTRUNC_LINK=""; \
            printf "pkg-config     = '%s'\n" "$(pkg-config --cflags --libs ${UNTRUNC_LINK:+"--${UNTRUNC_LINK}"} ${UNTRUNC_PKGCFG})"; \
            printf "libav=%s\n" "${LIBAV}" > /usr/local/share/untrunc/untrunc-info.txt; \
            g++ -o untrunc ${UNTRUNC_LINK:+"-${UNTRUNC_LINK}"} $(pkg-config --cflags ${UNTRUNC_LINK:+"--${UNTRUNC_LINK%-*}"} ${UNTRUNC_PKGCFG}) -I"${LIBAV_DIR}" *.cpp $(pkg-config --libs ${UNTRUNC_LINK:+"--${UNTRUNC_LINK%-*}"} ${UNTRUNC_PKGCFG}) ${UNTRUNC_ELIBS}; \
       fi \
    && mv -f untrunc /usr/local/bin/ \
    && { printf "%$(( ${COLUMNS:-80} - 4 ))s\n\n" "" | tr ' ' '-'; };


# Execute docker-untrunc.
ENTRYPOINT ["/usr/local/bin/untrunc"]




###########################
# Docker Production Image #
###########################
# Docker-Executable "docker-untrunc".
# The LABEL and ENV (but not ARG) are inherited from the Docker 'ubuntu' image not from 'construction' image.
FROM ubuntu AS production
# Be non-interactive during Docker image build.
ARG DEBIAN_FRONTEND=noninteractive
# Terminal used during Docker image build.
ARG TERM=linux

# OCI annotation of Untrunc.
LABEL org.opencontainers.title="Untrunc"
LABEL org.opencontainers.description="Restore a damaged (truncated) mp4, m4v, mov, 3gp video. Provided you have a similar not broken video. And some luck."
LABEL org.opencontainers.source="https://github.com/ponchio/untrunc"
LABEL org.opencontainers.licenses="GPL-2.0-or-later"
#LABEL org.opencontainers.image.documentation="<URL to get documentation on the image (how-to-use)>"
LABEL org.opencontainers.image.authors="Federico Ponchio <ponchio@gmail.com>"
#LABEL org.opencontainers.image.authors="https://github.com/ponchio/untrunc/graphs/contributors"
# Following OCI annotations are set from outside this Dockerfile,
# - so external commands can be used to retrieve the label information and
# - so no label will be set if there is no label information.
# label org.opencontainers.image.version="$(git describe --abbrev=8 2> /dev/null || git describe --tags --abbrev=8 2> /dev/null)"
# label org.opencontainers.image.revision="${TRAVIS_COMMIT:-"$(git log -n 1 --format='format:%H' 2> /dev/null)"}"
# label org.opencontainers.image.created="$(date -u '+%Y-%m-%dT%TZ')"


# Create a user for running without root privilages.
RUN groupadd -r untrunc && useradd --no-log-init -r -g untrunc untrunc
WORKDIR /home/untrunc

# Retrieve untrunc executable and data.
COPY --from=construction /usr/local/bin/untrunc*   /usr/local/bin/
COPY --from=construction /usr/local/share/untrunc/ /usr/local/share/untrunc/

# Install shared library packages.
RUN printf "\n%$(( ${COLUMNS:-80} - 4 ))s\n" "" | tr ' ' '-'; \
    if [ -f "/usr/local/share/untrunc/untrunc-pkgs.txt" ]; then \
        printf "untrunc-pkgs.txt: '%s'\n" "$(cat '/usr/local/share/untrunc/untrunc-pkgs.txt')"; \
        apt-get update \
        && apt-get install -y \
            $(cat '/usr/local/share/untrunc/untrunc-pkgs.txt'); \
    fi \
    && { rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /usr/share/doc/* /usr/share/man/* || true; } \
    && { printf "%$(( ${COLUMNS:-80} - 4 ))s\n\n" "" | tr ' ' '-'; };

# Run without root privilages.
USER untrunc


# Execute docker-untrunc.
ENTRYPOINT ["/usr/local/bin/untrunc"]

