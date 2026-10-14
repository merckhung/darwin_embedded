libxfce4util_apiname = 'xfce4util'
libxfce4util_pkgname = 'lib@0@-1.0'.format(libxfce4util_apiname)

util_headers_install_subdir = 'xfce4' / 'libxfce4util'
util_headers_install_dir = get_option('prefix') / get_option('includedir') / util_headers_install_subdir

util_headers = [
  'libxfce4util.h',
  'xfce-consolekit.h',
  'xfce-debug.h',
  'xfce-fileutils.h',
  'xfce-generics.h',
  'xfce-gio-extensions.h',
  'xfce-i18n.h',
  'xfce-kiosk.h',
  'xfce-license.h',
  'xfce-miscutils.h',
  'xfce-posix-signal-handler.h',
  'xfce-rc.h',
  'xfce-resource.h',
  'xfce-string.h',
  'xfce-systemd.h',
  'xfce-utf8.h',
]

util_gir_sources = [
  'libxfce4util-config.c',
  'xfce-consolekit.c',
  'xfce-fileutils.c',
  'xfce-gio-extensions.c',
  'xfce-i18n.c',
  'xfce-kiosk.c',
  'xfce-license.c',
  'xfce-miscutils.c',
  'xfce-posix-signal-handler.c',
  'xfce-rc-config.c',
  'xfce-rc-simple.c',
  'xfce-rc.c',
  'xfce-resource.c',
  'xfce-string.c',
  'xfce-systemd.c',
  'xfce-utf8.c',
]

util_sources = [
  'libxfce4util-private.c',
]
util_sources += util_gir_sources

util_config_data = configuration_data({
  'LIBXFCE4UTIL_VERSION_MAJOR': version_parts[0],
  'LIBXFCE4UTIL_VERSION_MINOR': version_parts[1],
  'LIBXFCE4UTIL_VERSION_MICRO': version_parts[2],
})
util_config_h = configure_file(
  configuration: util_config_data,
  input: 'libxfce4util-config.h.in',
  output: 'libxfce4util-config.h',
  install: true,
  install_dir: util_headers_install_dir,
)
# do not add this header to GI sources, it creates unsolvable conflicts with other
# libs whose identifier prefix is Xfce, e.g. libxfce4ui and libxfce4panel
util_sources += util_config_h

util_sources += custom_target(
  'libxfce4util-visibility.h',
  input: 'libxfce4util.symbols',
  output: 'libxfce4util-visibility.h',
  command: [xdt_gen_visibility, '--kind=header', '@INPUT@', '@OUTPUT@'],
)
util_sources += custom_target(
  'libxfce4util-visibility.c',
  input: 'libxfce4util.symbols',
  output: 'libxfce4util-visibility.c',
  command: [xdt_gen_visibility, '--kind=source', '@INPUT@', '@OUTPUT@'],
)

util_extra_args = [
  '-DDEFAULT_XDG_CACHE_HOME="~/.cache"',
  '-DDEFAULT_XDG_DATA_HOME="~/.local/share"',
  '-DDEFAULT_XDG_CONFIG_HOME="~/.config"',
  '-DDEFAULT_XDG_DATA_DIRS="@0@:/usr/local/share:/usr/share"'.format(datadir),
  '-DDEFAULT_XDG_CONFIG_DIRS="@0@/xdg:@0@/xfce4:/etc/xdg"'.format(sysconfdir),
  '-DKIOSKDIR="@0@/xdg/xfce4/kiosk"'.format(sysconfdir),
  '-DKIOSKDEF="ALL"',
  '-DLIBXFCE4UTIL_COMPILATION=1',
]

util_deps = [
  glib,
  gobject,
  gio,
]
if need_libintl
  util_deps += libintl
endif

libxfce4util = library(
  libxfce4util_apiname,
  sources: util_sources,
  version: libxfce4util_soversion,
  soversion: libxfce4util_soversion_major,
  c_args: util_extra_args,
  include_directories: [
    include_directories('..'),
  ],
  gnu_symbol_visibility: gnu_symbol_visibility,
  dependencies: util_deps,
  install: true,
)

install_headers(
  util_headers,
  subdir: util_headers_install_subdir,
)

pkgconfig.generate(
  libraries: [
    libxfce4util,
  ],
  version: meson.project_version(),
  name: libxfce4util_pkgname,
  filebase: libxfce4util_pkgname,
  description: 'Utility library for the Xfce 4 desktop environment',
  subdirs: [
    'xfce4',
  ],
  install_dir: get_option('prefix') / get_option('libdir') / 'pkgconfig',
)

if get_option('introspection')
  libxfce4util_gir = gnome.generate_gir(
    libxfce4util,
    sources: util_gir_sources + util_headers,
    namespace: 'Libxfce4util',
    nsversion: '1.0',
    identifier_prefix: 'Xfce',
    symbol_prefix: [
      'xfce',
    ],
    export_packages: libxfce4util_pkgname,
    includes: [
      'GLib-2.0',
      'GObject-2.0',
      'Gio-2.0',
    ],
    header: 'libxfce4util/libxfce4util.h',
    install: true,
    extra_args: [
      '-DLIBXFCE4UTIL_COMPILATION=1',
    ],
  )

  if generate_vala
    gnome.generate_vapi(
      libxfce4util_pkgname,
      install: true,
      packages: [
        'glib-2.0',
        'gobject-2.0',
        'gio-2.0',
      ],
      sources: libxfce4util_gir[0],
    )
  endif
endif

libxfce4util_dep = declare_dependency(
  dependencies: util_deps,
  link_with: libxfce4util,
)
