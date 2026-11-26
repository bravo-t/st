# st version
VERSION = 0.9.2

# Customize below to fit your system

# paths
# PREFIX = /usr/local
# DESKTOP_DIR = /usr/share/applications
# MANPREFIX = $(PREFIX)/share/man

PREFIX = $(HOME)/.local
DESKTOP_DIR = $(HOME)/.local/share/applications/
MANPREFIX = $(HOME)/.local/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

PKG_CONFIG = pkg-config

# includes and libs
INCS = -I$(X11INC) \
       `$(PKG_CONFIG) --cflags fontconfig` \
       `$(PKG_CONFIG) --cflags freetype2`
LIBS = -L$(X11LIB) -lm -lrt -lX11 -lutil -lXft -lXrender \
       `$(PKG_CONFIG) --libs fontconfig` \
       `$(PKG_CONFIG) --libs freetype2`

# Exceed TurboX (ETX) redraw pacing (etxmaxlatency/etxxfps in config.h) is
# chosen at run time: only an ETX X server (HCL_ThinX extension) gets it, so
# one binary serves ETX and local displays alike. To force it instead:
#ETXFLAGS = -DETX=1	# always use the ETX pacing
#ETXFLAGS = -DETX=0	# never use it
# (objects depend on this file, so editing it is enough to rebuild them)

# flags
CFLAGS = -std=c99 -O3
STCPPFLAGS = -DVERSION=\"$(VERSION)\" -D_XOPEN_SOURCE=600 $(ETXFLAGS)
STCFLAGS = $(INCS) $(STCPPFLAGS) $(CPPFLAGS) $(CFLAGS)
STLDFLAGS = $(LIBS) $(LDFLAGS)

# OpenBSD:
#CPPFLAGS = -DVERSION=\"$(VERSION)\" -D_XOPEN_SOURCE=600 -D_BSD_SOURCE
#LIBS = -L$(X11LIB) -lm -lX11 -lutil -lXft \
#       `$(PKG_CONFIG) --libs fontconfig` \
#       `$(PKG_CONFIG) --libs freetype2`
#MANPREFIX = ${PREFIX}/man

# compiler and linker
# CC = c99
