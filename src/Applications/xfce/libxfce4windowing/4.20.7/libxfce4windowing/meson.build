libxfce4windowing_apiname = 'xfce4windowing-@0@'.format(lib_so_version)
libxfce4windowing_pkgname = 'lib@0@'.format(libxfce4windowing_apiname)

windowing_headers = [
  'libxfce4windowing.h',
  'xfw-application.h',
  'xfw-monitor.h',
  'xfw-screen.h',
  'xfw-seat.h',
  'xfw-util.h',
  'xfw-window.h',
  'xfw-workspace-group.h',
  'xfw-workspace-manager.h',
  'xfw-workspace.h',
]

if enable_x11
  windowing_headers += 'xfw-x11.h'
endif

windowing_public_sources = [
  'libxfce4windowing-config.c',
  'xfw-application.c',
  'xfw-monitor.c',
  'xfw-screen.c',
  'xfw-seat.c',
  'xfw-util.c',
  'xfw-window.c',
  'xfw-workspace-group.c',
  'xfw-workspace-manager.c',
  'xfw-workspace.c',
]

windowing_sources = [
  'libxfce4windowing-private.c',
  'window-icon-utils.c',
  'xfw-gdk-private.c',
  'xfw-workspace-dummy.c',
  'xfw-workspace-group-dummy.c',
  'xfw-workspace-manager-dummy.c',
]
windowing_sources += windowing_public_sources

windowing_sources += gnome.genmarshal(
  'xfw-marshal',
  sources: 'xfw-marshal.list',
  prefix: 'xfw_marshal',
  internal: true,
  install_header: false,
)

windowing_config_data = configuration_data({
  'LIBXFCE4WINDOWING_VERSION_MAJOR': version_parts[0],
  'LIBXFCE4WINDOWING_VERSION_MINOR': version_parts[1],
  'LIBXFCE4WINDOWING_VERSION_MICRO': version_parts[2],
})

if enable_x11
  windowing_config_data.set('LIBXFCE4WINDOWING_HAS_X11', '#define LIBXFCE4WINDOWING_HAS_X11')
  windowing_sources += [
    'xfw-application-x11.c',
    'xfw-monitor-x11.c',
    'xfw-screen-x11.c',
    'xfw-window-x11.c',
    'xfw-wnck-icon.c',
    'xfw-workspace-manager-x11.c',
    'xfw-workspace-x11.c',
    'xsettings-x11.c',
  ]
  windowing_public_sources += 'xfw-window-x11.c'
else
  windowing_config_data.set('LIBXFCE4WINDOWING_HAS_X11', '/* #undef LIBXFCE4WINDOWING_HAS_X11 */')
endif

if enable_wayland
  windowing_config_data.set('LIBXFCE4WINDOWING_HAS_WAYLAND', '#define LIBXFCE4WINDOWING_HAS_WAYLAND')
  windowing_sources += wayland_protocols_generated_sources
  windowing_sources += [
    'xfw-application-wayland.c',
    'xfw-monitor-wayland.c',
    'xfw-screen-wayland.c',
    'xfw-seat-wayland.c',
    'xfw-window-wayland.c',
    'xfw-wl-raster-icon.c',
    'xfw-workspace-group-wayland.c',
    'xfw-workspace-manager-wayland.c',
    'xfw-workspace-wayland.c',
  ]
else
  windowing_config_data.set('LIBXFCE4WINDOWING_HAS_WAYLAND', '/* #undef LIBXFCE4WINDOWING_HAS_WAYLAND */')
endif

windowing_config_h = configure_file(
  configuration: windowing_config_data,
  input: 'libxfce4windowing-config.h.in',
  output: 'libxfce4windowing-config.h',
  install: true,
  install_dir: get_option('prefix') / get_option('includedir') / 'xfce4' / libxfce4windowing_pkgname / 'libxfce4windowing',
)
windowing_public_sources += windowing_config_h
windowing_sources += windowing_config_h

windowing_sources += configure_file(
  input: 'libxfce4windowing.symbols',
  output: 'libxfce4windowing-visibility.h',
  command: [xdt_gen_visibility, '--kind=header', '@INPUT@', '@OUTPUT@'],
)
windowing_sources += configure_file(
  input: 'libxfce4windowing.symbols',
  output: 'libxfce4windowing-visibility.c',
  command: [xdt_gen_visibility, '--kind=source', '@INPUT@', '@OUTPUT@'],
)

windowing_deps = [
  gio_unix,
  gtk,
  libintl,
  x11_deps,
  wayland_deps,
]

libxfce4windowing = library(
  libxfce4windowing_apiname,
  sources: windowing_sources,
  version: lib_version,
  soversion: lib_so_version,
  c_args: [
    '-DLIBXFCE4WINDOWING_COMPILATION=1',
    '-DWNCK_I_KNOW_THIS_IS_UNSTABLE=1',
  ],
  include_directories: [
    include_directories('..'),
  ],
  gnu_symbol_visibility: gnu_symbol_visibility,
  dependencies: windowing_deps,
  install: true,
)

install_headers(
  windowing_headers,
  subdir: 'xfce4' / libxfce4windowing_pkgname / 'libxfce4windowing',
)

pkgconfig.generate(
  libraries: [libxfce4windowing],
  version: meson.project_version(),
  name: libxfce4windowing_pkgname,
  filebase: libxfce4windowing_pkgname,
  description: 'X11/Wayland windowing utility library for Xfce',
  subdirs: ['xfce4' / libxfce4windowing_pkgname],
  install_dir: get_option('prefix') / get_option('libdir') / 'pkgconfig',
)

if enable_x11
  libxfce4windowing_x11_apiname = 'xfce4windowing-x11-@0@'.format(lib_so_version)
  libxfce4windowing_x11_pkgname = 'lib@0@'.format(libxfce4windowing_x11_apiname)

  pkgconfig.generate(
    libraries: [],
    version: meson.project_version(),
    name: libxfce4windowing_x11_pkgname,
    filebase: libxfce4windowing_x11_pkgname,
    description: 'X11/Wayland windowing utility library for Xfce (X11 backend)',
    requires_private: [libxfce4windowing_pkgname],
    install_dir: get_option('prefix') / get_option('libdir') / 'pkgconfig',
  )
endif

if get_option('introspection')
  windowing_gir_sources = windowing_public_sources + windowing_headers

  libxfce4windowing_gir = gnome.generate_gir(
    libxfce4windowing,
    sources: windowing_gir_sources,
    namespace: 'Libxfce4windowing',
    nsversion: '0.0',
    identifier_prefix: [
      'Xfw',
    ],
    symbol_prefix: [
      'libxfce4windowing',
      'xfw',
    ],
    export_packages: libxfce4windowing_pkgname,
    includes: [
      'GLib-2.0',
      'GObject-2.0',
      'Gio-2.0',
      'Gtk-3.0',
    ],
    header: 'libxfce4windowing/libxfce4windowing.h',
    install: true,
    extra_args: [
      '-DLIBXFCE4WINDOWING_COMPILATION=1',
    ],
  )

  libxfce4windowing_gir_dep = declare_dependency(sources: libxfce4windowing_gir)

  if generate_vala
    vapi_packages = [
      'gio-2.0',
      'gobject-2.0',
      'gtk+-3.0'
    ]

    if enable_x11
      vapi_packages += 'libwnck-3.0'
    endif

    libxfce4windowing_vapi = gnome.generate_vapi(
      libxfce4windowing_pkgname,
      sources: libxfce4windowing_gir[0],
      packages: vapi_packages,
      install: true,
    )
  endif
endif

libxfce4windowing_dep = declare_dependency(
  dependencies: windowing_deps,
  link_with: libxfce4windowing,
)
