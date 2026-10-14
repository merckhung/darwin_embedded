# vi:set ts=8 sw=8 noet ai nocindent syntax=automake:

lib_LTLIBRARIES = \
	libxfce4windowingui-0.la

libxfce4windowingui_0_includedir = $(includedir)/xfce4/libxfce4windowingui
libxfce4windowingui_0_headers = \
	libxfce4windowingui.h \
	libxfce4windowingui-config.h \
	xfw-window-action-menu.h \
	xfw-window-list-menu.h

libxfce4windowingui_0_include_HEADERS = \
	$(libxfce4windowingui_0_headers)

libxfce4windowingui_0_built_sources = \
	libxfce4windowingui-visibility.c \
	libxfce4windowingui-visibility.h

libxfce4windowingui_0_la_SOURCES = \
	$(libxfce4windowingui_0_headers) \
	libxfce4windowingui-config.c \
	xfw-window-action-menu.c \
	xfw-window-list-menu.c

nodist_libxfce4windowingui_0_la_SOURCES = \
	$(libxfce4windowingui_0_built_sources)

libxfce4windowingui_0_la_CFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir) \
	-DLIBXFCE4WINDOWINGUI_COMPILATION \
	-DLOCALEDIR=\"$(localedir)\" \
	-DDATADIR=\"$(datadir)\" \
	-DSYSCONFDIR=\"$(sysconfdir)\" \
	$(GOBJECT_CFLAGS) \
	$(GTK_CFLAGS)

libxfce4windowingui_0_la_LDFLAGS = \
	-version-info $(LIBXFCE4WINDOWINGUI_VERINFO) \
	-no-undefined

libxfce4windowingui_0_la_LIBADD = \
	$(top_builddir)/libxfce4windowing/libxfce4windowing-0.la \
	$(GOBJECT_LIBS) \
	$(GTK_LIBS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = \
	libxfce4windowingui-$(LIBXFCE4WINDOWING_VERSION_API).pc

EXTRA_DIST = \
	libxfce4windowingui.symbols \
	meson.build

BUILT_SOURCES = \
	$(libxfce4windowingui_0_built_sources)

CLEANFILES = \
	$(libxfce4windowingui_0_built_sources)

libxfce4windowingui-visibility.h: libxfce4windowingui.symbols Makefile
	$(AM_V_GEN) $(XDT_GEN_VISIBILITY) --kind=header $< $@

libxfce4windowingui-visibility.c: libxfce4windowingui.symbols Makefile
	$(AM_V_GEN) $(XDT_GEN_VISIBILITY) --kind=source $< $@

# GObject Introspection
-include $(INTROSPECTION_MAKEFILE)
INTROSPECTION_GIRS =
INTROSPECTION_SCANNER_ENV = CC="$(CC)"
INTROSPECTION_SCANNER_ARGS = \
	--add-include-path=$(srcdir) \
	--include-uninstalled=$(top_builddir)/libxfce4windowing/Libxfce4windowing-0.0.gir \
	--warn-all \
	--identifier-prefix=libxfce4windowingui \
	--identifier-prefix=Xfw \
	--c-include=libxfce4windowingui/libxfce4windowingui.h
INTROSPECTION_COMPILER_ARGS = \
	--includedir=$(srcdir) \
	--includedir=$(top_builddir)/libxfce4windowing \
	--includedir=.

if HAVE_INTROSPECTION

introspection_sources = $(libxfce4windowingui_0_la_SOURCES)

Libxfce4windowingui-0.0.gir: libxfce4windowingui-0.la Makefile
Libxfce4windowingui_0_0_gir_INCLUDES = GObject-2.0 GLib-2.0 Gio-2.0 Gtk-3.0
Libxfce4windowingui_0_0_gir_CFLAGS = $(libxfce4windowingui_0_la_CFLAGS)
Libxfce4windowingui_0_0_gir_LIBS = libxfce4windowingui-0.la $(top_builddir)/libxfce4windowing/libxfce4windowing-0.la
Libxfce4windowingui_0_0_gir_FILES = $(introspection_sources)
Libxfce4windowingui_0_0_gir_EXPORT_PACKAGES = libxfce4windowingui-0
INTROSPECTION_GIRS += Libxfce4windowingui-0.0.gir

girdir = $(datadir)/gir-1.0
gir_DATA = $(INTROSPECTION_GIRS)

typelibdir = $(libdir)/girepository-1.0
typelib_DATA = $(INTROSPECTION_GIRS:.gir=.typelib)

CLEANFILES += $(gir_DATA) $(typelib_DATA)

endif

# required for make distcheck (gtk-doc)
dist-hook: all
