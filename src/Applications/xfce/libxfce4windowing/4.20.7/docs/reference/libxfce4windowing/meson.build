if get_option('gtk-doc')
  libxfce4windowing_doc_name = 'libxfce4windowing'

  conf_data = configuration_data()
  conf_data.set_quoted('PACKAGE_NAME', libxfce4windowing_doc_name)
  conf_data.set_quoted('PACKAGE_VERSION', meson.project_version())
  conf_data.set_quoted('PACKAGE_STRING', '@0@-@1@'.format(libxfce4windowing_doc_name, meson.project_version()))

  configure_file(
    input: 'gtkdocentities.ent.in',
    output: 'gtkdocentities.ent',
    configuration: conf_data,
  )

  libxfce4windowing_doc_private_headers = [
    'libxfce4windowing-private.h',
    'libxfce4windowing-visibility.h',
    'xfw-application-private.h',
    'xfw-application-wayland.h',
    'xfw-application-x11.h',
    'xfw-gdk-private.h',
    'xfw-marshal.h',
    'xfw-monitor-private.h',
    'xfw-monitor-wayland.h',
    'xfw-monitor-x11.h',
    'xfw-screen-private.h',
    'xfw-screen-wayland.h',
    'xfw-screen-x11.h',
    'xfw-seat-private.h',
    'xfw-seat-wayland.h',
    'xfw-seat-x11.h',
    'xfw-window-private.h',
    'xfw-window-wayland.h',
    'xfw-window-x11.h',
    'xfw-wnck-icon.h',
    'xfw-workspace-dummy.h',
    'xfw-workspace-group-dummy.h',
    'xfw-workspace-group-private.h',
    'xfw-workspace-group-wayland.h',
    'xfw-workspace-manager-dummy.h',
    'xfw-workspace-manager-private.h',
    'xfw-workspace-manager-wayland.h',
    'xfw-workspace-manager-x11.h',
    'xfw-workspace-private.h',
    'xfw-workspace-wayland.h',
    'xfw-workspace-x11.h',
    'xsettings-x11.h',
  ]

  libxfce4windowing_doc_fixxref_args = [
    '--extra-dir=@0@/share/gtk-doc/html/glib'.format(glib.get_variable(pkgconfig: 'prefix')),
    '--extra-dir=@0@/share/gtk-doc/html/gobject'.format(gobject.get_variable(pkgconfig: 'prefix')),
    '--extra-dir=@0@/share/gtk-doc/html/gdk3'.format(gdk.get_variable(pkgconfig: 'prefix')),
    '--extra-dir=@0@/share/gtk-doc/html/gdk-pixbuf'.format(gdk_pixbuf.get_variable(pkgconfig: 'prefix')),
    # Weird non-standard Debian location?
    '--extra-dir=@0@/share/doc/libgdk-pixbuf2.0-doc/html/gdk-pixbuf'.format(gdk_pixbuf.get_variable(pkgconfig: 'prefix')),
  ]

  configure_file(
    configuration: configuration_data({
      'COPYRIGHT_YEAR': copyright_year,
    }),
    input: 'libxfce4windowing-docs.xml.in',
    output: 'libxfce4windowing-docs.xml',
    install: false,
  )

  gnome.gtkdoc(
    libxfce4windowing_doc_name,
    module_version: lib_so_version,
    src_dir: 'libxfce4windowing',
    ignore_headers: libxfce4windowing_doc_private_headers,
    main_xml: meson.project_build_root() / 'docs' / 'reference' / 'libxfce4windowing-docs.xml',
    dependencies: libxfce4windowing_dep,
    gobject_typesfile: 'libxfce4windowing.types',
    fixxref_args: libxfce4windowing_doc_fixxref_args,
    install: true,
    check: false,
  )
endif
