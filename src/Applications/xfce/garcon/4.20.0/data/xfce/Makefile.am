# vi:set ts=8 sw=8 noet ai nocindent syntax=automake:
#
# Copyright (c) 2010 Xfce Development Team
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library General Public License for more details.
#
# You should have received a copy of the GNU Library General
# Public License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA 02110-1301, USA.

menudir = $(sysconfdir)/xdg/menus
menu_DATA =								\
	xfce-applications.menu

directorydir = $(datadir)/desktop-directories
directory_in_files =							\
	xfce-accessories.desktop.in					\
	xfce-development.desktop.in					\
	xfce-education.desktop.in					\
	xfce-games.desktop.in						\
	xfce-graphics.desktop.in					\
	xfce-hardware.desktop.in					\
	xfce-multimedia.desktop.in					\
	xfce-network.desktop.in					\
	xfce-office.desktop.in					\
	xfce-other.desktop.in						\
	xfce-personal.desktop.in					\
	xfce-science.desktop.in					\
	xfce-screensavers.desktop.in					\
	xfce-settings.desktop.in					\
	xfce-system.desktop.in

directory_DATA = $(directory_in_files:.desktop.in=.directory)

# FIXME: xgettext doesn't recognize .directory.in files as desktop files so we
# use .desktop.in for the translation: is there a proper solution?
%.directory: %.desktop.in
	$(AM_V_GEN) $(MSGFMT) --desktop --template $< -d $(top_srcdir)/po -o $@

EXTRA_DIST =								\
	$(directory_in_files)						\
	$(menu_DATA)

DISTCLEANFILES =							\
	$(directory_DATA)
