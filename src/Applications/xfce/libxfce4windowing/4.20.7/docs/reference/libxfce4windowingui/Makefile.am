
# The name of the module.
DOC_MODULE = libxfce4windowingui

# The top-level SGML file.
DOC_MAIN_SGML_FILE = $(DOC_MODULE)-docs.xml

# Extra options to supply to gtkdoc-scan
SCAN_OPTIONS = --deprecated-guards="XFCE_DISABLE_DEPRECATED"

# The directory containing the source code. Relative to $(srcdir)
DOC_SOURCE_DIR = $(top_srcdir)/libxfce4windowingui

# Extra options to supply to gtkdoc-mkdb
MKDB_OPTIONS = --xml-mode --output-format=xml

# Extra options to supply to gtkdoc-fixref
FIXXREF_OPTIONS = \
	--extra-dir=$(top_builddir)/docs/reference/libxfce4windowing/html \
	--extra-dir=$(GLIB_PREFIX)/share/gtk-doc/html/glib \
	--extra-dir=$(GOBJECT_PREFIX)/share/gtk-doc/html/gobject \
	--extra-dir=$(GDK_PREFIX)/share/gtk-doc/html/gdk3 \
	--extra-dir=$(GTK_PREFIX)/share/gtk-doc/html/gtk3

# Used for dependencies
HFILE_GLOB = $(top_srcdir)/libxfce4windowingui/*.h
CFILE_GLOB = $(top_srcdir)/libxfce4windowingui/*.c

# Header files to ignore when scanning
IGNORE_HFILES = \
	libxfce4windowingui-visibility.h \
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
	-I$(top_builddir) \
	$(GTK_CFLAGS)
	
GTKDOC_LIBS = \
	$(top_builddir)/libxfce4windowingui/libxfce4windowingui-$(LIBXFCE4WINDOWINGUI_VERSION_API).la \
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
