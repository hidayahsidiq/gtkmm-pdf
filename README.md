i'm using codeblock as my IDE
using ucrt from msys as compiler 
 this how i setting lib on codeblock 


 install gtkmm using win-bright setting and add proppler from msys 
pacman -S --needed base-devel mingw-w64-x86_64-toolchain
pacman -S pkgconf
pacman -S mingw-w64-x86_64-gtk4
pacman -S mingw-w64-x86_64-gtkmm-4.0
pacman -S pkgconf
pacman -S mingw-w64-ucrt-x86_64-poppler
 
 Open Build Options -> Other Compiler Options -> add this :
`pkgconf --cflags gtk4`
`pkgconf --cflags glib-2.0`
`pkgconf --cflags glibmm-2.68`
`pkgconf --cflags gtkmm-4.0`
`pkgconf --cflags gdkmm-2.4`
`pkg-config --cflags gtkmm-4.0 poppler-glib`

Open Build Options -> linker settings -> other linker options -> add this :
`pkgconf --libs gtk4`
`pkgconf --libs glib-2.0`
`pkgconf --libs glibmm-2.68`
`pkgconf --libs gtkmm-4.0`
`pkgconf --libs gdkmm-2.4`
`pkg-config --libs gtkmm-4.0 poppler-glib`
-static-libgcc -static-libstdc++ -mwindows


