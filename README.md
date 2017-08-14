# iEmu
QEMU-s5l89xx-port (iPad-1) haxed together to compile for Windows.

Binaries are removed due to copyright n stuff.

Cygwin should work: ./configure --target-list=arm-softmmu --enable-sdl --enable-skinning --cross-prefix=i686-w64-mingw32- 

Extra libraries may need to be manually added because they aren't added automatically.
