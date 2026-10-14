if ENABLE_TESTS

noinst_PROGRAMS = \
	xfw-enum-monitors \
	xfw-enum-windows \
	xfw-enum-workspaces \
	xfw-monitor-offon

tests_cflags = \
	-I$(top_srcdir) \
	$(GTK_CFLAGS)

tests_ldadd = \
	$(GTK_LIBS) \
	$(top_builddir)/libxfce4windowing/libxfce4windowing-0.la

xfw_enum_monitors_SOURCES = xfw-enum-monitors.c
xfw_enum_monitors_CFLAGS = $(tests_cflags)
xfw_enum_monitors_LDADD = $(tests_ldadd)

xfw_enum_windows_SOURCES = xfw-enum-windows.c
xfw_enum_windows_CFLAGS = $(tests_cflags)
xfw_enum_windows_LDADD = $(tests_ldadd)

xfw_enum_workspaces_SOURCES = xfw-enum-workspaces.c
xfw_enum_workspaces_CFLAGS = $(tests_cflags)
xfw_enum_workspaces_LDADD = $(tests_ldadd)

xfw_monitor_offon_SOURCES = xfw-monitor-offon.c
xfw_monitor_offon_CFLAGS = $(tests_cflags)
xfw_monitor_offon_LDADD = $(tests_ldadd)

endif

EXTRA_DIST = \
	meson.build
