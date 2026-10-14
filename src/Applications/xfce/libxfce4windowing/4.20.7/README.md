[![License](https://img.shields.io/badge/License-LGPL%20v2-blue.svg)](https://gitlab.xfce.org/xfce/libxfce4windowing/-/blob/master/COPYING)

# libxfce4windowing

Libxfce4windowing is an abstraction library that attempts to present
windowing concepts (screens, toplevel windows, workspaces, etc.) in a
windowing-system-independent manner.

Currently, X11 is fully supported, via
[libwnck](https://gitlab.gnome.org/GNOME/libwnck).  Wayland is partially
supported, through various Wayland protocol extensions.  However, the
full range of operations available on X11 is not available on Wayland,
due to missing features in these protocol extensions.

Note: many of the links below do not yet work, as this is a new library
and its infrastructure is still being set up.

----

### Homepage

[Libxfce4windowing documentation](https://docs.xfce.org/xfce/libxfce4windowing/start)

### Changelog

See
[NEWS](https://gitlab.xfce.org/xfce/libxfce4windowing/-/blob/master/NEWS)
for details on changes and fixes made in the current release.

### Source Code Repository

[Libxfce4windowing source
code](https://gitlab.xfce.org/xfce/libxfce4windowing)

### Download a Release Tarball

[Libxfce4windowing
archive](https://archive.xfce.org/src/xfce/libxfce4windowing)
    or
[Libxfce4windowing
tags](https://gitlab.xfce.org/xfce/libxfce4windowing/-/tags)

### Installation

From source: 

    % cd libxfce4windowing
    % ./autogen.sh
    % make
    % make install

From release tarball:

    % tar xf libxfce4windowing-<version>.tar.bz2
    % cd libxfce4windowing-<version>
    % ./configure
    % make
    % make install

### Reporting Bugs

Visit the [reporting
bugs](https://docs.xfce.org/xfce/libxfce4windowing/bugs) page to view
currently open bug reports and instructions on reporting new bugs or
submitting bugfixes.
