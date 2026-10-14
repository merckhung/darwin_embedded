libxfce4windowingui_apiname = 'xfce4windowingui-@0@'.format(lib_so_version)
libxfce4windowingui_pkgname = 'lib@0@'.format(libxfce4windowingui_apiname)

windowingui_headers = [
  'libxfce4windowingui.h',
  'xfw-window-action-menu.h',
  'xfw-window-list-menu.h',
]

windowingui_gir_sources = [
  'xfw-window-action-menu.c',
  'xfw-window-list-menu.c',
]

windowingui_sources = windowingui_gir_sources
windowingui_sources += [
  'libxfce4windowingui-config.c',
]

windowingui_config_data = configuration_data({
  'LIBXFCE4WINDOWINGUI_VERSION_MAJOR': version_parts[0],
  'LIBXFCE4WINDOWINGUI_VERSION_MINOR': version_parts[1],
  'LIBXFCE4WINDOWINGUI_VERSION_MICRO': version_parts[2],
})

windowingui_sources += configure_file(
  configuration: windowingui_config_data,
  input: 'libxfce4windowingui-config.h.in',
  output: 'libxfce4windowingui-config.h',
  install: true,
  install_dir: get_option('prefix') / get_option('includedir') / 'xfce4' / libxfce4windowing_pkgname / 'libxfce4windowingui',
)

windowingui_sources += configure_file(
  input: 'libxfce4windowingui.symbols',
  output: 'libxfce4windowingui-visibility.h',
  command: [xdt_gen_visibility, '--kind=header', '@INPUT@', '@OUTPUT@'],
)
windowingui_sources += configure_file(
  input: 'libxfce4windowingui.symbols',
  output: 'libxfce4windowingui-visibility.c',
  command: [xdt_gen_visibility, '--kind=source', '@INPUT@', '@OUTPUT@'],
)

windowingui_deps = [
  gtk,
  libintl,
]

libxfce4windowingui = library(
  libxfce4windowingui_apiname,
  sources: windowingui_sources,
  version: lib_version,
  soversion: lib_so_version,
  c_args: [
    '-DLIBXFCE4WINDOWINGUI_COMPILATION=1',
  ],
  include_directories: [
    include_directories('..'),
  ],
  gnu_symbol_visibility: gnu_symbol_visibility,
  dependencies: windowingui_deps,
  link_with: [
    libxfce4windowing,
  ],
  install: true,
)

install_headers(
  windowingui_headers,
  subdir: 'xfce4' / libxfce4windowing_pkgname / 'libxfce4windowingui',
)

pkgconfig.generate(
  libraries: [libxfce4windowingui],
  version: meson.project_version(),
  name: libxfce4windowingui_pkgname,
  filebase: libxfce4windowingui_pkgname,
  description: 'X11/Wayland windowing utility library for Xfce - extra widgets',
  subdirs: ['xfce4' / libxfce4windowing_pkgname],
  install_dir: get_option('prefix') / get_option('libdir') / 'pkgconfig',
)

if get_option('introspection')
  libxfce4windowingui_gir = gnome.generate_gir(
    libxfce4windowingui,
    sources: windowingui_gir_sources + windowingui_headers,
    namespace: 'Libxfce4windowingui',
    nsversion: '0.0',
    identifier_prefix: [
      'Xfw',
    ],
    symbol_prefix: [
      'libxfce4windowingui',
      'xfw',
    ],
    export_packages: libxfce4windowingui_pkgname,
    includes: [
      'GLib-2.0',
      'GObject-2.0',
      'Gio-2.0',
      'Gtk-3.0',
      libxfce4windowing_gir[0],
    ],
    header: 'libxfce4windowingui/libxfce4windowingui.h',
    dependencies: libxfce4windowing_gir_dep,
    install: true,
    extra_args: [
      '-DLIBXFCE4WINDOWINGUI_COMPILATION=1',
    ],
  )

  if generate_vala
    gnome.generate_vapi(
      libxfce4windowingui_pkgname,
      sources: libxfce4windowingui_gir[0],
      packages: [
        'gio-2.0',
        'gobject-2.0',
        'gtk+-3.0',
        libxfce4windowing_vapi,
      ],
      install: true,
    )
  endif
endif

libxfce4windowingui_dep = declare_dependency(
  dependencies: windowingui_deps,
  link_with: libxfce4windowingui,
)
