@SET_MAKE@

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = \
	protocols \
	libxfce4windowing \
	libxfce4windowingui \
	docs \
	po \
	tests

EXTRA_DIST = \
	$(XDT_GEN_VISIBILITY_DIST) \
	README.md \
	m4/introspection.m4 \
	meson.build \
	meson_options.txt \
	po/meson.build \
	xfce-revision.h.in

DISTCLEANFILES =

distuninstallcheck_listfiles = \
	find . -type f -print | grep -v ./share/icons/hicolor/icon-theme.cache

distclean-local:
	-rm -rf *.cache *~

.PHONY: ChangeLog

ChangeLog: Makefile
	(GIT_DIR=$(top_srcdir)/.git git log > .changelog.tmp \
	&& mv .changelog.tmp ChangeLog; rm -f .changelog.tmp) \
	|| (touch ChangeLog; echo 'Git directory not found: installing possibly empty changelog.' >&2)

dist-hook: ChangeLog

compile_flags.txt: $(shell find . -name Makefile)
	rm -f $@
	for flag in \
		-DHAVE_CONFIG_H \
		-DLIBXFCE4WINDOWING_COMPILATION \
		-DLIBXFCE4WINDOWINGUI_COMPILATION \
		-DWNCK_I_KNOW_THIS_IS_UNSTABLE \
		-I$(top_srcdir) \
		-I$(top_srcdir)/libxfce4windowing \
		-I$(top_builddir)/libxfce4windowing \
		$(GOBJECT_CFLAGS) \
		$(GIO_UNIX_CFLAGS) \
		$(GTK_CFLAGS) \
		$(LIBWNCK_CFLAGS) \
		$(WAYLAND_CLIENT_CFLAGS) \
	; do \
		echo "$$flag" >> $@ || exit 1; \
	done

AM_DISTCHECK_CONFIGURE_FLAGS = \
	--enable-gtk-doc \
	--enable-introspection
