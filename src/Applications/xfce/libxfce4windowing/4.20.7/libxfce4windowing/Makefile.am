# vi:set ts=8 sw=8 noet ai nocindent syntax=automake:

lib_LTLIBRARIES = \
	libxfce4windowing-0.la

libxfce4windowing_0_includedir = $(includedir)/xfce4/libxfce4windowing
libxfce4windowing_0_headers = \
	libxfce4windowing.h \
	libxfce4windowing-config.h \
	xfw-application.h \
	xfw-monitor.h \
	xfw-screen.h \
	xfw-seat.h \
	xfw-util.h \
	xfw-window.h \
	xfw-workspace-group.h \
	xfw-workspace-manager.h \
	xfw-workspace.h

if ENABLE_X11
libxfce4windowing_0_headers += \
	xfw-x11.h
endif

libxfce4windowing_0_public_sources = \
	$(libxfce4windowing_0_headers) \
	libxfce4windowing-config.c \
	xfw-application.c \
	xfw-monitor.c \
	xfw-screen.c \
	xfw-seat.c \
	xfw-util.c \
	xfw-window.c \
	xfw-workspace-group.c \
	xfw-workspace-manager.c \
	xfw-workspace.c

libxfce4windowing_0_include_HEADERS = \
	$(libxfce4windowing_0_headers)

libxfce4windowing_0_built_sources = \
	libxfce4windowing-visibility.c \
	libxfce4windowing-visibility.h \
	xfw-marshal.c \
	xfw-marshal.h

libxfce4windowing_0_x11_sources = \
	xfw-application-x11.c \
	xfw-application-x11.h \
	xfw-monitor-x11.c \
	xfw-monitor-x11.h \
	xfw-screen-x11.c \
	xfw-screen-x11.h \
	xfw-window-x11.c \
	xfw-window-x11.h \
	xfw-wnck-icon.c \
	xfw-wnck-icon.h \
	xfw-workspace-manager-x11.c \
	xfw-workspace-manager-x11.h \
	xfw-workspace-x11.c \
	xfw-workspace-x11.h \
	xsettings-x11.c \
	xsettings-x11.h

libxfce4windowing_0_wayland_sources = \
	xfw-application-wayland.c \
	xfw-application-wayland.h \
	xfw-monitor-wayland.c \
	xfw-monitor-wayland.h \
	xfw-screen-wayland.c \
	xfw-screen-wayland.h \
	xfw-seat-wayland.c \
	xfw-seat-wayland.h \
	xfw-window-wayland.c \
	xfw-window-wayland.h \
	xfw-wl-raster-icon.c \
	xfw-wl-raster-icon.h \
	xfw-workspace-group-wayland.c \
	xfw-workspace-group-wayland.h \
	xfw-workspace-manager-wayland.c \
	xfw-workspace-manager-wayland.h \
	xfw-workspace-wayland.c \
	xfw-workspace-wayland.h

libxfce4windowing_0_la_SOURCES = \
	$(libxfce4windowing_0_public_sources) \
	libxfce4windowing-private.c \
	libxfce4windowing-private.h \
	window-icon-utils.c \
	window-icon-utils.h \
	xfw-application-private.h \
	xfw-gdk-private.c \
	xfw-gdk-private.h \
	xfw-monitor-private.h \
	xfw-screen-private.h \
	xfw-seat-private.h \
	xfw-window-private.h \
	xfw-workspace-dummy.c \
	xfw-workspace-dummy.h \
	xfw-workspace-group-dummy.c \
	xfw-workspace-group-dummy.h \
	xfw-workspace-group-private.h \
	xfw-workspace-manager-dummy.c \
	xfw-workspace-manager-dummy.h \
	xfw-workspace-manager-private.h \
	xfw-workspace-private.h

if ENABLE_X11
libxfce4windowing_0_la_SOURCES += \
	$(libxfce4windowing_0_x11_sources)
endif

if ENABLE_WAYLAND
libxfce4windowing_0_la_SOURCES += \
	$(libxfce4windowing_0_wayland_sources)
endif

nodist_libxfce4windowing_0_la_SOURCES = \
	$(libxfce4windowing_0_built_sources)

libxfce4windowing_0_la_CFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir) \
	-DLIBXFCE4WINDOWING_COMPILATION \
	-DWNCK_I_KNOW_THIS_IS_UNSTABLE \
	-DLOCALEDIR=\"$(localedir)\" \
	-DDATADIR=\"$(datadir)\" \
	-DSYSCONFDIR=\"$(sysconfdir)\" \
	$(GIO_UNIX_CFLAGS) \
	$(GOBJECT_CFLAGS) \
	$(GTK_CFLAGS) \
	$(LIBWNCK_CFLAGS) \
	$(LIBX11_CFLAGS) \
	$(DISPLAY_INFO_CFLAGS) \
	$(XRANDR_CFLAGS) \
	$(WAYLAND_CLIENT_CFLAGS)

libxfce4windowing_0_la_LDFLAGS = \
	-version-info $(LIBXFCE4WINDOWING_VERINFO) \
	-no-undefined \
	$(LIBX11_LDFLAGS)

libxfce4windowing_0_la_LIBADD = \
	$(GOBJECT_LIBS) \
	$(GIO_UNIX_LIBS) \
	$(GTK_LIBS) \
	$(LIBWNCK_LIBS) \
	$(LIBX11_LIBS) \
	$(DISPLAY_INFO_LIBS) \
	$(XRANDR_LIBS) \
	$(WAYLAND_CLIENT_LIBS)

if ENABLE_WAYLAND
libxfce4windowing_0_la_LIBADD += \
	$(top_builddir)/protocols/libwayland-protocols.la
endif

pkgconfig_files = \
	libxfce4windowing-$(LIBXFCE4WINDOWING_VERSION_API).pc
if ENABLE_X11
pkgconfig_files += \
	libxfce4windowing-x11-$(LIBXFCE4WINDOWING_VERSION_API).pc
endif

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = $(pkgconfig_files)

EXTRA_DIST = \
	libxfce4windowing.symbols \
	meson.build \
	xfw-marshal.list

BUILT_SOURCES = \
	$(libxfce4windowing_0_built_sources)

CLEANFILES = \
	$(libxfce4windowing_0_built_sources)

xfw-marshal.h: $(top_srcdir)/libxfce4windowing/xfw-marshal.list Makefile
	$(AM_V_GEN) glib-genmarshal --prefix=xfw_marshal --internal --header $< > $@

xfw-marshal.c: $(top_srcdir)/libxfce4windowing/xfw-marshal.list Makefile
	$(AM_V_GEN) echo '#include "xfw-marshal.h"' > $@
	$(AM_V_GET) glib-genmarshal --prefix=xfw_marshal --body $< >> $@

libxfce4windowing-visibility.h: libxfce4windowing.symbols Makefile
	$(AM_V_GEN) $(XDT_GEN_VISIBILITY) --kind=header $< $@

libxfce4windowing-visibility.c: libxfce4windowing.symbols Makefile
	$(AM_V_GEN) $(XDT_GEN_VISIBILITY) --kind=source $< $@

# GObject Introspection
-include $(INTROSPECTION_MAKEFILE)
INTROSPECTION_GIRS =
INTROSPECTION_SCANNER_ENV = CC="$(CC)"
INTROSPECTION_SCANNER_ARGS = \
	--add-include-path=$(srcdir) \
	--warn-all \
	--identifier-prefix=libxfce4windowing \
	--identifier-prefix=Xfw \
	--c-include=libxfce4windowing/libxfce4windowing.h
INTROSPECTION_COMPILER_ARGS = \
	--includedir=$(srcdir) \
	--includedir=.

if HAVE_INTROSPECTION

introspection_sources = $(libxfce4windowing_0_public_sources)

Libxfce4windowing-0.0.gir: libxfce4windowing-0.la Makefile
Libxfce4windowing_0_0_gir_INCLUDES = GObject-2.0 GLib-2.0 Gio-2.0 Gtk-3.0
Libxfce4windowing_0_0_gir_CFLAGS = $(libxfce4windowing_0_la_CFLAGS)
Libxfce4windowing_0_0_gir_LIBS = libxfce4windowing-0.la
Libxfce4windowing_0_0_gir_FILES = $(introspection_sources)
Libxfce4windowing_0_0_gir_EXPORT_PACKAGES = libxfce4windowing-0
INTROSPECTION_GIRS += Libxfce4windowing-0.0.gir

girdir = $(datadir)/gir-1.0
gir_DATA = $(INTROSPECTION_GIRS)

typelibdir = $(libdir)/girepository-1.0
typelib_DATA = $(INTROSPECTION_GIRS:.gir=.typelib)

CLEANFILES += $(gir_DATA) $(typelib_DATA)

endif

# required for make distcheck (gtk-doc)
dist-hook: all
