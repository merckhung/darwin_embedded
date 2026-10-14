project(
  'libxfce4windowing',
  'c',
version : '4.20.7',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.57.0',
  default_options : ['c_std=gnu11', 'buildtype=debugoptimized', 'warning_level=2']
)

project_namespace = 'xfce'
pkgdatadir = get_option('prefix') / get_option('datadir') / meson.project_name()
copyright_year = '2026'

cc = meson.get_compiler('c')
pkgconfig = import('pkgconfig')
gnome = import('gnome')
i18n = import('i18n')
fs = import('fs')

dependency_versions = {
  'display-info': '>= 0.1.1',
  'gir': '>= 1.72.0',
  'glib': '>= 2.72.0',
  'gdk-pixbuf': '>= 2.42.8',
  'gtk': '>= 3.24.10',
  'libx11': '>= 1.6.7',
  'wayland': '>= 1.20',
  'wayland-protocols': '>= 1.25',
  'wlr-protocols': '>= 0',
  'wnck': '>= 3.14',
  'xrandr': '>= 1.5.0',
}

glib = dependency('glib-2.0', version: dependency_versions['glib'])
gir = dependency('gobject-introspection-1.0', version: dependency_versions['gir'], required: get_option('introspection'))
gobject = dependency('gobject-2.0', version: dependency_versions['glib'])
gio_unix = dependency('gio-unix-2.0', version: dependency_versions['glib'])
gdk = dependency('gdk-3.0', version: dependency_versions['gtk'])
gdk_pixbuf = dependency('gdk-pixbuf-2.0', version: dependency_versions['gdk-pixbuf'])
gtk = dependency('gtk+-3.0', version: dependency_versions['gtk'])
python3 = find_program('python3', required: true)

xdt_gen_visibility = find_program('xdt-gen-visibility', required: true)

# Feature: 'x11'
x11_deps = []
x11_deps += dependency('libdisplay-info', version: dependency_versions['display-info'], required: get_option('x11'))
x11_deps += dependency('x11', version: dependency_versions['libx11'], required: get_option('x11'))
x11_deps += dependency('gdk-x11-3.0', version: dependency_versions['gtk'], required: get_option('x11'))
x11_deps += dependency('libwnck-3.0', version: dependency_versions['wnck'], required: get_option('x11'))
x11_deps += dependency('xrandr', version: dependency_versions['xrandr'], required: get_option('x11'))

# Feature: 'wayland'
wayland_deps = []
wayland_deps += dependency('gdk-wayland-3.0', version: dependency_versions['gtk'], required: get_option('wayland'))
wayland_deps += dependency('wayland-client', version: dependency_versions['wayland'], required: get_option('wayland'))
wayland_build_deps = []
wayland_build_deps += dependency('wayland-scanner', version: dependency_versions['wayland'], required: get_option('wayland'), native: true)
wayland_scanner = wayland_build_deps[-1]
wayland_build_deps += dependency('wayland-protocols', version: dependency_versions['wayland-protocols'], required: get_option('wayland'))
wayland_protocols = wayland_build_deps[-1]

wlr_protocols = dependency('wlr-protocols', version: dependency_versions['wlr-protocols'], required: false)

feature_cflags = []

enable_x11 = not get_option('x11').disabled()
foreach dep : x11_deps
  enable_x11 = enable_x11 and dep.found()
endforeach
if enable_x11
  feature_cflags += '-DENABLE_X11=1'
endif

enable_wayland = not get_option('wayland').disabled()
foreach dep : wayland_deps
  enable_wayland = enable_wayland and dep.found()
endforeach
foreach dep : wayland_build_deps
  enable_wayland = enable_wayland and dep.found()
endforeach
if enable_wayland
  feature_cflags += '-DENABLE_WAYLAND=1'

  wayland_scanner_path = wayland_scanner.get_variable(pkgconfig: 'wayland_scanner')
  wayland_scanner_bin = find_program(wayland_scanner_path, required: true)

  wl_protocols_pkgdatadir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')

  if wlr_protocols.found()
    wlr_protocols_pkgdatadir = wlr_protocols.get_variable(pkgconfig: 'pkgdatadir')
  else
    wlr_protocols_pkgdatadir = meson.project_source_root() / 'protocols' / 'wlr-protocols'
  endif

  # FIXME: Bump wayland-protocols to 1.39 when it is an acceptable requirement,
  # and remove this and protocols/ext-workspace-v1.xml
  if fs.is_file(wl_protocols_pkgdatadir / 'staging' / 'ext-workspace' / 'ext-workspace-v1.xml')
    ext_workspace_xml = wl_protocols_pkgdatadir / 'staging' / 'ext-workspace' / 'ext-workspace-v1.xml'
  else
    ext_workspace_xml = 'ext-workspace-v1.xml'
  endif
endif

if not enable_x11 and not enable_wayland
  error('At least one of the X11 and Wayland backends must be enabled')
endif

vapigen = find_program('vapigen', required: get_option('vala'))
if get_option('vala').enabled()
  assert(get_option('introspection'), 'Building Vala bindings requires building GObject introspection data')
endif
generate_vala = not get_option('vala').disabled() and vapigen.found()

gnu_symbol_visibility = 'default'
if get_option('visibility')
  gnu_symbol_visibility = 'hidden'
  feature_cflags += '-DENABLE_SYMBOL_VISIBILITY=1'
endif

if cc.has_function('bind_textdomain_codeset')
  feature_cflags += '-DHAVE_BIND_TEXTDOMAIN_CODESET=1'
  libintl = dependency('', required: false)
else
  libintl = cc.find_library('intl', required: false)
  if libintl.found() and cc.has_function('bind_textdomain_codeset', dependencies: libintl)
    feature_cflags += '-DHAVE_BIND_TEXTDOMAIN_CODESET=1'
  endif
endif

extra_cflags = []
extra_cflags_check = [
  '-Wmissing-declarations',
  '-Wmissing-noreturn',
  '-Wold-style-definition',
  '-Wredundant-decls',
  '-Wpointer-arith',
  '-Wcast-align',
  '-Winit-self',
  '-Wshadow',
  '-Wmissing-include-dirs',
  '-Wundef',
  '-Wformat',
  '-Wformat-security',
  '-Wformat-y2k',
  '-Wnested-externs',
  '-Wno-unused-parameter',
  '-Wno-declaration-after-statement',
  '-Werror=implicit-function-declaration',
  '-Wno-error=deprecated-declarations',
]

optimization = get_option('optimization')
if get_option('debug') and optimization in ['0', 'g']
  extra_cflags_check += '-fstack-protector-strong'
  extra_cflags += [
    '-DDEBUG=1',
    '-DDEBUG_TRACE=1',
    '-DG_ENABLE_DEBUG',
  ]
elif optimization in ['3', 's']
  extra_cflags += [
    '-DNDEBUG',
    '-DG_DISABLE_CAST_CHECKS',
    '-DG_DISABLE_ASSERT',
  ]
endif

if dependency_versions.has_key('glib')
  glib_version_parts = dependency_versions['glib'].split(' ')
  glib_min_version_parts = glib_version_parts[1].split('.')
  glib_min_version_define = 'GLIB_VERSION_@0@_@1@'.format(glib_min_version_parts[0], glib_min_version_parts[1])
  extra_cflags += [
    '-DGLIB_VERSION_MIN_REQUIRED=@0@'.format(glib_min_version_define),
    '-DGLIB_VERSION_MAX_ALLOWED=@0@'.format(glib_min_version_define),
    '-DG_LOG_DOMAIN="@0@"'.format(meson.project_name()),
    '-DG_LOG_USE_STRUCTURED=1',
  ]
endif

version_parts = meson.project_version().split('-dev')[0].split('.')
version_short = '@0@.@1@'.format(version_parts[0], version_parts[1])

extra_cflags += [
  '-DPACKAGE="@0@"'.format(meson.project_name()),
  '-DPACKAGE_NAME="@0@"'.format(meson.project_name()),
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DVERSION_SHORT="@0@"'.format(version_short),
  '-DPACKAGE_STRING="@0@ @1@"'.format(meson.project_name(), meson.project_version()),
  '-DPACKAGE_DATADIR="@0@"'.format(pkgdatadir),
  '-DCOPYRIGHT_YEAR="@0@"'.format(copyright_year),
  '-DPACKAGE_LOCALE_DIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  '-DPACKAGE_BUGREPORT="https://gitlab.xfce.org/@0@/@1@/-/issues"'.format(project_namespace, meson.project_name()),
  '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()),
  '-DPREFIX="@0@"'.format(get_option('prefix')),
  '-DBINDIR="@0@"'.format(get_option('prefix') / get_option('bindir')),
  '-DDATADIR="@0@"'.format(get_option('prefix') / get_option('datadir')),
  '-DINCLUDEDIR="@0@"'.format(get_option('prefix') / get_option('includedir')),
  '-DLIBDIR="@0@"'.format(get_option('prefix') / get_option('libdir')),
  '-DLIBEXECDIR="@0@"'.format(get_option('prefix') / get_option('libexecdir')),
  '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  '-DLOCALSTATEDIR="@0@"'.format(get_option('prefix') / get_option('localstatedir')),
  '-DSBINDIR="@0@"'.format(get_option('prefix') / get_option('sbindir')),
  '-DSYSCONFDIR="@0@"'.format(get_option('prefix') / get_option('sysconfdir')),
  '-DHAVE_XFCE_REVISION_H=1',
]

add_project_arguments(cc.get_supported_arguments(extra_cflags_check), language: 'c')
add_project_arguments(extra_cflags, language: 'c')
add_project_arguments(feature_cflags, language: 'c')

xfce_revision_h = vcs_tag(
  command: ['git', 'rev-parse', '--short', 'HEAD'],
  fallback: 'UNKNOWN',
  input: 'xfce-revision.h.in',
  output: 'xfce-revision.h',
  replace_string: '@REVISION@',
)

lib_so_version = '0'
lib_version = '@0@.0.0'.format(lib_so_version)

subdir('protocols')
subdir('libxfce4windowing')
subdir('libxfce4windowingui')
subdir('docs' / 'reference' / 'libxfce4windowing')
subdir('docs' / 'reference' / 'libxfce4windowingui')
subdir('po')
if get_option('tests')
  subdir('tests')
endif
