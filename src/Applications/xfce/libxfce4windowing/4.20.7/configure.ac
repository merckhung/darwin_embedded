dnl
dnl Copyright (c) 2022
dnl         The Xfce development team. All rights reserved.
dnl
dnl libxfce4windowing -- utilities for handling different windowing environments
dnl

dnl version info
m4_define([libxfce4windowing_verinfo], [0:0:0])
m4_define([libxfce4windowing_version_api], [0])
m4_define([libxfce4windowingui_verinfo], [0:0:0])
m4_define([libxfce4windowingui_version_api], [0])
m4_define([copyright_year], [2026])
XDT_VERSION_INIT([4.20.7])

dnl minimum required versions
m4_define([libx11_minimum_version], [1.6.7])
m4_define([glib_minimum_version], [2.72.0])
m4_define([gtk_minimum_version], [3.24.10])
m4_define([gdk_pixbuf_minimum_version], [2.42.8])
m4_define([wnck_minimum_version], [3.14])
m4_define([wayland_minimum_version], [1.20])
m4_define([wayland_protocols_minimum_version], [1.25])
m4_define([display_info_minimum_version], [0.1.1])
m4_define([xrandr_minimum_version], [1.5.0])

dnl init autoconf
AC_COPYRIGHT([Copyright (c) 2022-copyright_year() The Xfce development team. All rights reserved.])
AC_INIT([libxfce4windowing], [xdt_version], [https://gitlab.xfce.org/xfce/libxfce4windowing])
AC_PREREQ([2.69])
AC_CONFIG_MACRO_DIRS([m4])
AC_REVISION([xdt_version_build])
COPYRIGHT_YEAR=copyright_year()
AC_DEFINE_UNQUOTED([COPYRIGHT_YEAR], ["$COPYRIGHT_YEAR"], [Copyright year])
AC_SUBST([COPYRIGHT_YEAR])

dnl init automake
AM_INIT_AUTOMAKE([1.11 dist-bzip2 tar-ustar no-dist-gzip foreign])
AC_CONFIG_HEADERS([config.h])
AM_SILENT_RULES([yes])

dnl check for UNIX variants
AC_USE_SYSTEM_EXTENSIONS

dnl check for basic programs
AC_PROG_CC()
LT_PATH_LD([])
AM_PROG_CC_C_O()
AC_PROG_INSTALL()

dnl prepare libtool
LT_PREREQ([2.4])
LT_INIT([disable-static])

dnl version info
LIBXFCE4WINDOWING_VERINFO=libxfce4windowing_verinfo()
LIBXFCE4WINDOWING_VERSION_API=libxfce4windowing_version_api()
LIBXFCE4WINDOWING_VERSION_MAJOR=xdt_version_major()
LIBXFCE4WINDOWING_VERSION_MINOR=xdt_version_minor()
LIBXFCE4WINDOWING_VERSION_MICRO=xdt_version_micro()
LIBXFCE4WINDOWINGUI_VERINFO=libxfce4windowingui_verinfo()
LIBXFCE4WINDOWINGUI_VERSION_API=libxfce4windowingui_version_api()
LIBXFCE4WINDOWINGUI_VERSION_MAJOR=xdt_version_major()
LIBXFCE4WINDOWINGUI_VERSION_MINOR=xdt_version_minor()
LIBXFCE4WINDOWINGUI_VERSION_MICRO=xdt_version_micro()
AC_SUBST([LIBXFCE4WINDOWING_VERINFO])
AC_SUBST([LIBXFCE4WINDOWING_VERSION_API])
AC_SUBST([LIBXFCE4WINDOWING_VERSION_MAJOR])
AC_SUBST([LIBXFCE4WINDOWING_VERSION_MINOR])
AC_SUBST([LIBXFCE4WINDOWING_VERSION_MICRO])
AC_SUBST([LIBXFCE4WINDOWINGUI_VERINFO])
AC_SUBST([LIBXFCE4WINDOWINGUI_VERSION_API])
AC_SUBST([LIBXFCE4WINDOWINGUI_VERSION_MAJOR])
AC_SUBST([LIBXFCE4WINDOWINGUI_VERSION_MINOR])
AC_SUBST([LIBXFCE4WINDOWINGUI_VERSION_MICRO])

dnl check for standard header files
AC_CHECK_HEADERS([ctype.h errno.h fcntl.h math.h pwd.h signal.h \
                  string.h sys/param.h sys/stat.h  \
                  sys/types.h time.h \
                  unistd.h])

GETTEXT_PACKAGE="$PACKAGE"
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE], ["$GETTEXT_PACKAGE"], [Name of default gettext domain])
AC_SUBST([GETTEXT_PACKAGE])
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.19.8])
AC_SEARCH_LIBS([bind_textdomain_codeset], [intl],
    [AC_DEFINE([HAVE_BIND_TEXTDOMAIN_CODESET], [1], [Define to 1 if you have the 'bind_textdomain_codeset' function.])],
    [])

dnl required
XDT_CHECK_PACKAGE([GLIB], [glib-2.0], [glib_minimum_version])
XDT_CHECK_PACKAGE([GOBJECT], [gobject-2.0], [glib_minimum_version])
XDT_CHECK_PACKAGE([GIO_UNIX], [gio-unix-2.0], [glib_minimum_version])
XDT_CHECK_PACKAGE([GDK], [gdk-3.0], [gtk_minimum_version])
XDT_CHECK_PACKAGE([GDK_PIXBUF], [gdk-pixbuf-2.0], [gdk_pixbuf_minimum_version])
XDT_CHECK_PACKAGE([GTK], [gtk+-3.0], [gtk_minimum_version])

GLIB_PREFIX="`$PKG_CONFIG --variable=prefix glib-2.0`"
GOBJECT_PREFIX="`$PKG_CONFIG --variable=prefix gobject-2.0`"
GDK_PREFIX="`$PKG_CONFIG --variable=prefix gdk-3.0`"
GDK_PIXBUF_PREFIX="`$PKG_CONFIG --variable=prefix gdk-pixbuf-2.0`"
GTK_PREFIX="`$PKG_CONFIG --variable=prefix gtk+-3.0`"

AC_SUBST([GLIB_PREFIX])
AC_SUBST([GOBJECT_PREFIX])
AC_SUBST([GDK_PREFIX])
AC_SUBST([GDK_PIXBUF_PREFIX])
AC_SUBST([GTK_PREFIX])

XDT_CHECK_OPTIONAL_FEATURE([X11],
                           [x11],
                           [
                               XDT_FEATURE_DEPENDENCY([LIBX11], [x11], [libx11_minimum_version])
                               XDT_FEATURE_DEPENDENCY([GDK_X11], [gdk-x11-3.0], [gtk_minimum_version])
                               XDT_FEATURE_DEPENDENCY([LIBWNCK], [libwnck-3.0], [wnck_minimum_version])
                               XDT_FEATURE_DEPENDENCY([DISPLAY_INFO], [libdisplay-info], [display_info_minimum_version])
                               XDT_FEATURE_DEPENDENCY([XRANDR], [xrandr], [xrandr_minimum_version])
                           ],
                           [the X11 windowing system])
XDT_CHECK_OPTIONAL_FEATURE([WAYLAND],
                           [wayland],
                           [
                               XDT_FEATURE_DEPENDENCY([GDK_WAYLAND], [gdk-wayland-3.0], [gtk_minimum_version])
                               XDT_FEATURE_DEPENDENCY([WAYLAND_SCANNER], [wayland-scanner], [wayland_minimum_version])
                               XDT_FEATURE_DEPENDENCY([WAYLAND_CLIENT], [wayland-client], [wayland_minimum_version])
                               XDT_FEATURE_DEPENDENCY([WAYLAND_PROTOCOLS], [wayland-protocols], [wayland_protocols_minimum_version])
                           ],
                           [the Wayland windowing system])

if test x"$ENABLE_X11" != x"yes" -a x"$ENABLE_WAYLAND" != x"yes"; then
    AC_MSG_ERROR([At least one windowing backend must be enabled])
fi

AS_IF([test x"$ENABLE_X11" = x"yes"],
      [
          LIBXFCE4WINDOWING_HAS_X11="#define LIBXFCE4WINDOWING_HAS_X11"
      ],
      [
          LIBXFCE4WINDOWING_HAS_X11="/* #undef LIBXFCE4WINDOWING_HAS_X11 */"
      ])

AS_IF([test x"$ENABLE_WAYLAND" = x"yes"],
      [
          XDT_CHECK_PACKAGE_BINARY([WAYLAND_SCANNER], [wayland-scanner], [wayland_scanner], [wayland-scanner])

          WL_PROTOCOLS_PKGDATADIR=`$PKG_CONFIG --variable=pkgdatadir wayland-protocols`
          AC_SUBST([WL_PROTOCOLS_PKGDATADIR])

          WLR_PROTOCOLS_PKGDATADIR=`$PKG_CONFIG --variable=pkgdatadir wlr-protocols 2>/dev/null || true`
          if test x"$WLR_PROTOCOLS_PKGDATADIR" = x""; then
              WLR_PROTOCOLS_PKGDATADIR="$ac_abs_confdir/protocols/wlr-protocols"
              if test "$(echo "$WLR_PROTOCOLS_PKGDATADIR/"*)" = "$WLR_PROTOCOLS_PKGDATADIR/*"; then
                  AC_MSG_ERROR([$WLR_PROTOCOLS_PKGDATADIR seems empty, did you clone the wlr-protocols submodule?])
              fi
          fi
          AC_MSG_CHECKING([for wlr-protocols])
          AC_MSG_RESULT([$WLR_PROTOCOLS_PKGDATADIR])
          AC_SUBST([WLR_PROTOCOLS_PKGDATADIR])

          LIBXFCE4WINDOWING_HAS_WAYLAND="#define LIBXFCE4WINDOWING_HAS_WAYLAND"
      ],
      [
          LIBXFCE4WINDOWING_HAS_WAYLAND="/* #undef LIBXFCE4WINDOWING_HAS_WAYLAND */"
      ])

dnl FIXME: Bump wayland_protocols_minimum_version to 1.39 when it is an acceptable requirement,
dnl and remove this and protocols/ext-workspace-v1.xml
AM_CONDITIONAL([HAVE_WORKSPACE], [test -f "$WL_PROTOCOLS_PKGDATADIR/staging/ext-workspace/ext-workspace-v1.xml"])

AC_SUBST([LIBXFCE4WINDOWING_HAS_X11])
AC_SUBST([LIBXFCE4WINDOWING_HAS_WAYLAND])

AC_ARG_ENABLE([tests],
              AS_HELP_STRING([--enable-tests],
                             [Build some small test programs (default=disabled)]),
              [enable_tests=$enableval], [enable_tests=no])
AM_CONDITIONAL([ENABLE_TESTS], [test x"$enable_tests" = x"yes"])

GTK_DOC_CHECK([1.30])

GOBJECT_INTROSPECTION_CHECK([1.72.0])

dnl check for debugging support
XDT_FEATURE_DEBUG([xdt_debug_default])
XDT_FEATURE_LINKER_OPTS()

dnl ****************************************
dnl *** Check for ELF visibility support ***
dnl ****************************************
AC_PATH_PROG([XDT_GEN_VISIBILITY], [xdt-gen-visibility], [], [$PWD:$PATH])
if test x"$XDT_GEN_VISIBILITY" = x""; then
    AC_MSG_ERROR([Unable to find xdt-gen-visibility in your PATH. Ensure that a recent enough version of xfce4-dev-tools is installed and try again.])
elif test x"$XDT_GEN_VISIBILITY" = x"$PWD/xdt-gen-visibility"; then
    XDT_GEN_VISIBILITY_DIST="xdt-gen-visibility"
else
    XDT_GEN_VISIBILITY_DIST=""
fi
AC_SUBST([XDT_GEN_VISIBILITY_DIST])
AC_SUBST([XDT_GEN_VISIBILITY])

AC_ARG_ENABLE([visibility],
              AS_HELP_STRING([--disable-visibility],
                             [Do not use ELF visibility attributes]),
              [enable_visibility=$enableval], [enable_visibility=yes])
have_gnuc_visibility=no
if test "x$enable_visibility" != "xno"; then
  XDT_SUPPORTED_FLAGS([xdt_vis_test_cflags], [-Wall -Werror -Wno-unused-parameter -fvisibility=hidden])
  saved_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS $xdt_vis_test_cflags"
  AC_MSG_CHECKING([whether $CC supports the GNUC visibility attribute])
  AC_COMPILE_IFELSE([AC_LANG_SOURCE(
  [
    void test_default(void);
    void test_hidden(void);
    void __attribute__((visibility("default"))) test_default(void) {}
    void __attribute__((visibility("hidden"))) test_hidden(void) {}
    int main(int argc, char **argv) {
      test_default();
      test_hidden();
      return 0;
    }
  ])],
  [
    have_gnuc_visibility=yes
    AC_MSG_RESULT([yes])
  ],
  [
    AC_MSG_RESULT([no])
  ])
  CFLAGS="$saved_CFLAGS"
fi
if test "x$have_gnuc_visibility" = "xyes"; then
  CPPFLAGS="$CPPFLAGS -DENABLE_SYMBOL_VISIBILITY=1"
  CFLAGS="$CFLAGS -fvisibility=hidden"
fi

AC_CONFIG_FILES([
Makefile
docs/Makefile
docs/reference/Makefile
docs/reference/libxfce4windowing/Makefile
docs/reference/libxfce4windowingui/Makefile
libxfce4windowing/Makefile
libxfce4windowing/libxfce4windowing-0.pc
libxfce4windowing/libxfce4windowing-config.h
libxfce4windowing/libxfce4windowing-x11-0.pc
libxfce4windowingui/Makefile
libxfce4windowingui/libxfce4windowingui-0.pc
libxfce4windowingui/libxfce4windowingui-config.h
po/Makefile.in
protocols/Makefile
tests/Makefile
])
AC_OUTPUT

echo
echo "Build Configuration:"
echo "* X11 support:                   $ENABLE_X11"
echo "* Wayland support:               $ENABLE_WAYLAND"
echo "* Debug support:                 $enable_debug"
echo "* GObject Introspection support: $enable_introspection"
echo "* Documentation support:         $enable_gtk_doc"
echo "* Test programs:                 $enable_tests"
echo
