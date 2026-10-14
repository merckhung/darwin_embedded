dnl vi:set et ai sw=2 sts=2 ts=2: */
dnl
dnl Copyright (c) 2007-2011 Jannis Pohlmann <jannis@xfce.org>
dnl Copyright (c) 2012-2022 The Xfce development team
dnl
dnl This library is free software; you can redistribute it and/or
dnl modify it under the terms of the GNU Library General Public
dnl License as published by the Free Software Foundation; either
dnl version 2 of the License, or (at your option) any later version.
dnl
dnl This library is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
dnl GNU Library General Public License for more details.
dnl
dnl You should have received a copy of the GNU Library General
dnl Public License along with this library; if not, write to the
dnl Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
dnl Boston, MA 02110-1301, USA.

dnl ***************************
dnl *** Version information ***
dnl ***************************
m4_define([garcon_verinfo], [0:0:0])
m4_define([garcon_version_api_major], [1])
XDT_VERSION_INIT([4.20.0])

dnl minimum required versions
m4_define([glib_min_version], [2.72.0])
m4_define([gtk_min_version], [3.24.0])
m4_define([libxfce4util_min_version], [4.15.6])
m4_define([libxfce4ui_min_version], [4.15.7])

dnl ***************************
dnl *** Initialize autoconf ***
dnl ***************************
AC_COPYRIGHT([Copyright (c) 2007-2024 The Xfce development team. All rights reserved.
  Written for Xfce by Jannis Pohlmann <jannis@xfce.org>.])
AC_INIT([garcon], [xdt_version], [https://gitlab.xfce.org/xfce/garcon])
AC_PREREQ([2.69])
AC_CONFIG_MACRO_DIRS([m4])
AC_REVISION([xdt_version_build])

dnl ***************************
dnl *** Initialize automake ***
dnl ***************************
AM_INIT_AUTOMAKE([1.15 dist-bzip2 tar-ustar no-dist-gzip foreign])
AC_CONFIG_HEADERS([config.h])
AM_MAINTAINER_MODE()
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl **************************
dnl *** Libtool versioning ***
dnl **************************
GARCON_VERINFO=garcon_verinfo()
AC_SUBST([GARCON_VERINFO])

dnl ****************************
dnl *** Subst garcon version ***
dnl ****************************
GARCON_VERSION_MAJOR=xdt_version_major()
GARCON_VERSION_MINOR=xdt_version_minor()
GARCON_VERSION_MICRO=xdt_version_micro()
GARCON_VERSION_API_MAJOR=garcon_version_api_major()
GARCON_VERSION_API=$GARCON_VERSION_API_MAJOR
AC_SUBST([GARCON_VERSION_MAJOR])
AC_SUBST([GARCON_VERSION_MINOR])
AC_SUBST([GARCON_VERSION_MICRO])
AC_SUBST([GARCON_VERSION_API_MAJOR])
AC_SUBST([GARCON_VERSION_API])

dnl ********************************
dnl *** Check for basic programs ***
dnl ********************************
AM_PROG_AS()
AC_PROG_CC()
AC_PROG_INSTALL()
AM_PROG_CC_C_O()

dnl **************************
dnl *** Initialize libtool ***
dnl **************************
LT_PREREQ([2.2.6])
LT_INIT([disable-static])

dnl ***************************************
dnl *** Check for standard header files ***
dnl ***************************************
AC_CHECK_HEADERS([fcntl.h errno.h sys/mman.h sys/stat.h sys/wait.h memory.h \
                  stdlib.h stdio.h string.h sys/types.h sys/time.h unistd.h \
                  time.h stdarg.h sys/types.h sys/uio.h sched.h ctype.h])

dnl ************************************
dnl *** Check for standard functions ***
dnl ************************************
AC_FUNC_MMAP()
AC_CHECK_FUNCS([bind_textdomain_codeset realpath])

dnl ******************************
dnl *** Check for i18n support ***
dnl ******************************
GETTEXT_PACKAGE="$PACKAGE"
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE], ["$GETTEXT_PACKAGE"], [Name of default gettext domain])
AC_SUBST([GETTEXT_PACKAGE])
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.19.8])

dnl ***********************************
dnl *** Check for required packages ***
dnl ***********************************
XDT_CHECK_PACKAGE([GLIB], [glib-2.0], [glib_min_version])
XDT_CHECK_PACKAGE([GIO], [gio-2.0], [glib_min_version])
XDT_CHECK_PACKAGE([GOBJECT], [gobject-2.0], [glib_min_version])
XDT_CHECK_PACKAGE([GTHREAD], [gthread-2.0], [glib_min_version])
XDT_CHECK_PACKAGE([GTK3], [gtk+-3.0], [gtk_min_version])
XDT_CHECK_PACKAGE([LIBXFCE4UTIL], [libxfce4util-1.0], [libxfce4util_min_version])
XDT_CHECK_PACKAGE([LIBXFCE4UI2], [libxfce4ui-2], [libxfce4ui_min_version])

dnl *************************
dnl *** Check for gtk-doc ***
dnl *************************
GTK_DOC_CHECK([1.20])

dnl ***************************************
dnl *** Check for GObject Introspection ***
dnl ***************************************
GOBJECT_INTROSPECTION_CHECK([1.72.0])

dnl ***********************************
dnl *** Check for debugging support ***
dnl ***********************************
XDT_FEATURE_DEBUG([xdt_debug_default])

dnl **************************************
dnl *** Check for linker optimizations ***
dnl **************************************
XDT_FEATURE_LINKER_OPTS()

AC_CONFIG_FILES([
Makefile
data/Makefile
data/xfce/Makefile
docs/Makefile
docs/reference/Makefile
garcon/Makefile
garcon/garcon-1.pc
garcon/garcon-config.h
garcon-gtk/Makefile
garcon-gtk/garcon-gtk3-1.pc
icons/Makefile
icons/32x32/Makefile
icons/64x64/Makefile
po/Makefile.in
tests/Makefile
])
AC_OUTPUT

dnl ***************************
dnl *** Print configuration ***
dnl ***************************
echo
echo "Build Configuration:"
echo
echo "* Debug Support:                 $enable_debug"
echo "* GObject Introspection support: $enable_introspection"
echo
