Untrunc
=======

Restore a damaged (truncated) mp4, m4v, mov, 3gp video. Provided you have a similar not broken video. And some luck. Also works the same way with audio like m4a.

You need:

* Another video file (taken from the same camera/app) which isn't broken
* [Libav 12](https://libav.org/)
* Basic ability to use a command line

## Installing on CentOS 7

    yum install https://extras.getpagespeed.com/release-el7-latest.rpm
    yum install untrunc

## Installing from git

Type the folling commands to build from git :
```bash
git clone --recurse-submodules https://github.com/ponchio/untrunc
cd untrunc/libav
./configure
make
cd ..
g++ -o untrunc -I./libav file.cpp main.cpp track.cpp atom.cpp codec_*.cpp codecstats.cpp codec.cpp mp4.cpp log.cpp -L./libav/libavformat -lavformat -L./libav/libavcodec -lavcodec -L./libav/libavresample -lavresample -L./libav/libavutil -lavutil -lpthread -lz -std=c++11
sudo install -vpm 755 ./untrunc /usr/local/bin/ 
which -a untrunc
```

Depending on your system and Libav configure options you might need to add extra flags to the command line:
- add `-lbz2`   for errors like `undefined reference to 'BZ2_bzDecompressInit'`,
- add `-llzma`  for errors like `undefined reference to 'lzma_stream_decoder'`,
- add `-lX11`   for errors like `undefined reference to 'XOpenDisplay'`,
- add `-lvdpau` for errors like `undefined reference to 'VDPAU...'`,
- add `-ldl`    for errors like `undefined reference to 'dlopen'`.

On macOS add the following (tested on OSX 10.12.6):
- add `-framework CoreFoundation -framework CoreVideo -framework VideoDecodeAcceleration`.

## Installing from zip files

Because Untrunc uses Libav internal headers and internal headers are not included in application development packages, you must build Libav from source.

Download the Libav sources from either [the download page](https://libav.org/download/) or its [GitHub mirror](https://github.com/libav/libav/releases):

    wget https://libav.org/releases/libav-12.3.tar.xz
    [or:  wget https://github.com/libav/libav/archive/v12.3.zip ]

Download the source code from GitHub at https://github.com/ponchio/untrunc

    wget https://github.com/ponchio/untrunc/archive/master.zip

Unzip the Untrunc source code:

    unzip master.zip

    
Unzip the Libav source code into the Untrunc source directory with either:

    tar -xJf libav-12.3.tar.xz -C untrunc-master
    [or:  unzip v12.3.zip -d untrunc-master ]

Go into the libav directory and build it:

    cd untrunc-master

    cd untrunc-master/libav-12.3/
    ./configure
    make
    cd ..

Depending on your system you may need to install additional packages if configure complains about them (eg. libz, libbz2, liblzma, libdl, libvdpau, libX11)
If `configure` complains about `nasm/yasm not found`, you can either install Nasm or Yasm or tell `configure` not to use a stand-alone assembler with `--disable-yasm`.

Build the untrunc executable:

    g++ -o untrunc -I./libav-12.3 file.cpp main.cpp track.cpp atom.cpp codec_*.cpp codecstats.cpp codec.cpp mp4.cpp log.cpp -L./libav-12.3/libavformat -lavformat -L./libav-12.3/libavcodec -lavcodec -L./libav-12.3/libavresample -lavresample -L./libav-12.3/libavutil -lavutil -lpthread -lz

Depending on your system and Libav configure options you might need to add extra flags to the command line:
- add `-lbz2`   for errors like `undefined reference to 'BZ2_bzDecompressInit'`,
- add `-llzma`  for errors like `undefined reference to 'lzma_stream_decoder'`,
- add `-lX11`   for errors like `undefined reference to 'XOpenDisplay'`,
- add `-lvdpau` for errors like `undefined reference to 'VDPAU...'`,
- add `-ldl`    for errors like `undefined reference to 'dlopen'`.

On macOS add the following (tested on OSX 10.12.6):
- add `-framework CoreFoundation -framework CoreVideo -framework VideoDecodeAcceleration`.

### Mac OSX

Follow the above steps for "Installing on other operating system", but use the following g++ command:

	g++ -o untrunc file.cpp main.cpp track.cpp atom.cpp codec_*.cpp codecstats.cpp codec.cpp mp4.cpp log.cpp -I./libav-12.3 -L./libav-12.3/libavformat -lavformat -L./libav-12.3/libavcodec -lavcodec -L./libav-12.3/libavresample -lavresample -L./libav-12.3/libavutil -lavutil -lpthread -lz -framework CoreFoundation -framework CoreVideo -framework VideoDecodeAcceleration -lbz2 -DOSX

## Arch package

Jose1711 kindly provides an arch package here: https://aur.archlinux.org/packages/untrunc-git/

## Using

You need both the broken video and an example working video (ideally from the same camera, if not the chances to fix it are slim).

Run this command in the folder where you have unzipped and compiled Untrunc but replace the `/path/to/...` bits with your 2 video files:

    ./untrunc /path/to/working-video.m4v /path/to/broken-video.m4v

Then it should churn away and hopefully produce a playable file called `broken-video_fixed.m4v`.

That's it you're done!

(Thanks to Tom Sparrow for providing the guide)

## Docker container

You can use the included Dockerfile to build and execute the package as a container (you might need to add docker group: sudo usermod -a -G docker $USER, and you might want to add the --network=host option in case of   "Temporary failure resolving")
```
docker build -t untrunc .
```
Then e.g. to fix the file `/path/to/videos/broken_video` use this command to mount your video-folder into the folder `/files`
inside the docker container and call `untrunc` also inside the docker container:
```
docker run -v /path/to/videos/:/files untrunc /files/working_video /files/broken_video
```

### Help/Support

If you managed to recover the video, help me to find time to keep working on this software and make other people happy.
If you didn't, I need more corrupted samples to improve the program and I might solve the issue, who knows... so write me.

Donations can be made at my page, http://vcg.isti.cnr.it/~ponchio/untrunc.php, and promptly converted into beer.

Thank you.
