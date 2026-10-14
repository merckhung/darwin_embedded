executable(
  'xfce4-kiosk-query',
  [
    'main.c',
  ],
  sources: xfce_revision_h,
  include_directories: [
    include_directories('..'),
  ],
  dependencies: [
    glib,
  ],
  link_with: [
    libxfce4util,
  ],
  install: true,
  install_dir: get_option('prefix') / get_option('sbindir'),
)
