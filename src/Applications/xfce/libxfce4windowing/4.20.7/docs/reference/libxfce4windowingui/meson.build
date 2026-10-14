if get_option('gtk-doc')
  libxfce4windowingui_doc_name = 'libxfce4windowingui'

  conf_data = configuration_data()
  conf_data.set_quoted('PACKAGE_NAME', libxfce4windowingui_doc_name)
  conf_data.set_quoted('PACKAGE_VERSION', meson.project_version())
  conf_data.set_quoted('PACKAGE_STRING', '@0@-@1@'.format(libxfce4windowingui_doc_name, meson.project_version()))

  configure_file(
    input: 'gtkdocentities.ent.in',
    output: 'gtkdocentities.ent',
    configuration: conf_data,
  )

  libxfce4windowingui_doc_private_headers = [
    'libxfce4windowingui-visibility.h',
  ]

  libxfce4windowingui_doc_fixxref_args = [
    '--extra-dir=@0@/share/gtk-doc/html/glib'.format(glib.get_variable(pkgconfig: 'prefix')),
    '--extra-dir=@0@/share/gtk-doc/html/gobject'.format(gobject.get_variable(pkgconfig: 'prefix')),
    '--extra-dir=@0@/share/gtk-doc/html/gdk3'.format(gdk.get_variable(pkgconfig: 'prefix')),
    '--extra-dir=@0@/share/gtk-doc/html/gtk3'.format(gtk.get_variable(pkgconfig: 'prefix')),
  ]

  configure_file(
    configuration: configuration_data({
      'COPYRIGHT_YEAR': copyright_year,
    }),
    input: 'libxfce4windowingui-docs.xml.in',
    output: 'libxfce4windowingui-docs.xml',
    install: false,
  )

  gnome.gtkdoc(
    libxfce4windowingui_doc_name,
    module_version: lib_so_version,
    src_dir: 'libxfce4windowingui',
    ignore_headers: libxfce4windowingui_doc_private_headers,
    main_xml: meson.project_build_root() / 'docs' / 'reference' / 'libxfce4windowingui-docs.xml',
    dependencies: libxfce4windowingui_dep,
    gobject_typesfile: 'libxfce4windowingui.types',
    fixxref_args: libxfce4windowingui_doc_fixxref_args,
    install: true,
    check: false,
  )
endif
