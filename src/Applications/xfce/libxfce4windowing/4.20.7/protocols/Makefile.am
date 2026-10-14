if ENABLE_WAYLAND

noinst_LTLIBRARIES = \
	libwayland-protocols.la

libwayland_protocols_built_sources = \
	ext-workspace-v1.c \
	ext-workspace-v1-client.h \
	wlr-foreign-toplevel-management-unstable-v1.c \
	wlr-foreign-toplevel-management-unstable-v1-client.h \
	xdg-output-unstable-v1.c \
	xdg-output-unstable-v1-client.h \
	xfce-foreign-toplevel-management-private-v1.c \
	xfce-foreign-toplevel-management-private-v1-client.h \
	xfce-output-private-v1.c \
	xfce-output-private-v1-client.h

nodist_libwayland_protocols_la_SOURCES = \
	$(libwayland_protocols_built_sources)

libwayland_protocols_la_CFLAGS = \
	$(WAYLAND_CLIENT_CFLAGS)

libwayland_protocols_la_LDFLAGS = \
	-Wl,-no-undefined

libwayland_protocols_la_LIBADD = \
	$(WAYLAND_CLIENT_LIBS)

xdg-output-unstable-v1.c: $(WL_PROTOCOLS_PKGDATADIR)/unstable/xdg-output/xdg-output-unstable-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) private-code $< $@

xdg-output-unstable-v1-client.h: $(WL_PROTOCOLS_PKGDATADIR)/unstable/xdg-output/xdg-output-unstable-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) client-header $< $@

if HAVE_WORKSPACE
ext-workspace-v1.c: $(WL_PROTOCOLS_PKGDATADIR)/staging/ext-workspace/ext-workspace-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) private-code $< $@

ext-workspace-v1-client.h: $(WL_PROTOCOLS_PKGDATADIR)/staging/ext-workspace/ext-workspace-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) client-header $< $@
else
ext-workspace-v1.c: ext-workspace-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) private-code $< $@

ext-workspace-v1-client.h: ext-workspace-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) client-header $< $@
endif

xfce-foreign-toplevel-management-private-v1.c: xfce-wayland-protocols/xfce-foreign-toplevel-management-private-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) private-code $< $@

xfce-foreign-toplevel-management-private-v1-client.h: xfce-wayland-protocols/xfce-foreign-toplevel-management-private-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) client-header $< $@

xfce-output-private-v1.c: xfce-wayland-protocols/xfce-output-private-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) private-code $< $@

xfce-output-private-v1-client.h: xfce-wayland-protocols/xfce-output-private-v1.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) client-header $< $@

%.c: $(WLR_PROTOCOLS_PKGDATADIR)/unstable/%.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) private-code $< $@

%-client.h: $(WLR_PROTOCOLS_PKGDATADIR)/unstable/%.xml
	$(AM_V_GEN) $(WAYLAND_SCANNER) client-header $< $@


CLEANFILES = \
	$(libwayland_protocols_built_sources)

BUILT_SOURCES = \
	$(libwayland_protocols_built_sources)

endif

EXTRA_DIST = \
	meson.build \
	wlr-protocols/unstable/wlr-foreign-toplevel-management-unstable-v1.xml \
	ext-workspace-v1.xml \
	xfce-wayland-protocols/xfce-foreign-toplevel-management-private-v1.xml \
	xfce-wayland-protocols/xfce-output-private-v1.xml \
	$(NULL)

# required for make distcheck (gtk-doc)
dist-hook: all
