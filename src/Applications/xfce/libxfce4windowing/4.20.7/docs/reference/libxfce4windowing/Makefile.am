
# The name of the module.
DOC_MODULE = libxfce4windowing

# The top-level SGML file.
DOC_MAIN_SGML_FILE = $(DOC_MODULE)-docs.xml

# Extra options to supply to gtkdoc-scan
SCAN_OPTIONS = --deprecated-guards="XFCE_DISABLE_DEPRECATED"

# The directory containing the source code. Relative to $(srcdir)
DOC_SOURCE_DIR = $(top_srcdir)/libxfce4windowing

# Extra options to supply to gtkdoc-mkdb
MKDB_OPTIONS = --xml-mode --output-format=xml

# Extra options to supply to gtkdoc-fixref
FIXXREF_OPTIONS = \
	--extra-dir=$(GLIB_PREFIX)/share/gtk-doc/html/glib \
	--extra-dir=$(GOBJECT_PREFIX)/share/gtk-doc/html/gobject \
	--extra-dir=$(GDK_PREFIX)/share/gtk-doc/html/gdk3 \
	--extra-dir=$(GDK_PIXBUF_PREFIX)/share/gtk-doc/html/gdk-pixbuf

# Weird non-standard Debian location?
FIXXREF_OPTIONS += \
	--extra-dir=$(GDK_PIXBUF_PREFIX)/share/doc/libgdk-pixbuf2.0-doc/html/gdk-pixbuf

# Used for dependencies
HFILE_GLOB = $(top_srcdir)/libxfce4windowing/*.h
CFILE_GLOB = $(top_srcdir)/libxfce4windowing/*.c

# Header files to ignore when scanning
IGNORE_HFILES = \
	libxfce4windowing-private.h \
	libxfce4windowing-visibility.h \
	xfw-application-private.h \
	xfw-application-wayland.h \
	xfw-application-x11.h \
	xfw-gdk-private.h \
	xfw-marshal.h \
	xfw-monitor-private.h \
	xfw-monitor-wayland.h \
	xfw-monitor-x11.h \
	xfw-screen-private.h \
	xfw-screen-wayland.h \
	xfw-screen-x11.h \
	xfw-seat-private.h \
	xfw-seat-wayland.h \
	xfw-seat-x11.h \
	xfw-window-private.h \
	xfw-window-wayland.h \
	xfw-window-x11.h \
	xfw-wnck-icon.h \
	xfw-workspace-dummy.h \
	xfw-workspace-group-dummy.h \
	xfw-workspace-group-private.h \
	xfw-workspace-group-wayland.h \
	xfw-workspace-manager-dummy.h \
	xfw-workspace-manager-private.h \
	xfw-workspace-manager-wayland.h \
	xfw-workspace-manager-x11.h \
	xfw-workspace-private.h \
	xfw-workspace-wayland.h \
	xfw-workspace-x11.h \
	xsettings-x11.h \
	$(NULL)

# Extra files to add when scanning (relative to $srcdir)
EXTRA_HFILES =

# Images to copy into HTML directory
HTML_IMAGES =

# Extra SGML files that are included by DOC_MAIN_SGML_FILE
content_files =

# CFLAGS and LDFLAGS for compiling scan program. Only needed
# if $(DOC_MODULE).types is non-empty.
GTKDOC_CFLAGS = \
	-I$(top_srcdir) \
	-DWNCK_I_KNOW_THIS_IS_UNSTABLE \
	$(GDK_CFLAGS) \
	$(LIBWNCK_CFLAGS) \
	$(WAYLAND_CLIENT_CFLAGS)
	
GTKDOC_LIBS = \
	$(top_builddir)/libxfce4windowing/libxfce4windowing-$(LIBXFCE4WINDOWING_VERSION_API).la

include $(top_srcdir)/gtk-doc.make

$(top_builddir)/docs/reference/$(DOC_MODULE)-docs.xml: $(DOC_MODULE)-docs.xml.in
	$(AM_V_GEN) $(SED) -e "s,\@COPYRIGHT_YEAR\@,@COPYRIGHT_YEAR@,g" $< >$@

EXTRA_DIST += \
	gtkdocentities.ent.in \
	$(DOC_MODULE)-docs.xml.in \
	meson.build

DISTCLEANFILES = \
	$(DOC_MODULE)-docs.xml \
	$(NULL)

# vi:set ts=8 sw=8 noet ai nocindent syntax=automake:
