project('hicolor-icon-theme',
  version: '0.18',
  meson_version: '>=0.60',
)

pkgc = import('pkgconfig')

icon_sizes = [
  '16x16',
  '22x22',
  '24x24',
  '32x32',
  '36x36',
  '48x48',
  '64x64',
  '72x72',
  '96x96',
  '128x128',
  '192x192',
  '256x256',
  '512x512',
  '16x16@2',
  '22x22@2',
  '24x24@2',
  '32x32@2',
  '36x36@2',
  '48x48@2',
  '64x64@2',
  '72x72@2',
  '96x96@2',
  '128x128@2',
  '192x192@2',
  '256x256@2',
  '512x512@2',
  'scalable',
]

icon_contexts = [
  'actions',
  'animations',
  'apps',
  'categories',
  'devices',
  'emblems',
  'emotes',
  'filesystems',
  'intl',
  'mimetypes',
  'places',
  'status',
  'stock',
]

stock_subs = [
  'chart',
  'code',
  'data',
  'form',
  'image',
  'io',
  'media',
  'navigation',
  'net',
  'object',
  'table',
  'text',
]

theme_dir = get_option('datadir') / 'icons/hicolor'

install_data('index.theme',
  install_dir: theme_dir,
)

foreach dir : icon_sizes
  foreach ctxt : icon_contexts
    install_emptydir(theme_dir / dir / ctxt)
    if ctxt == 'stock'
      foreach sub : stock_subs
        install_emptydir(theme_dir / dir / ctxt / sub)
      endforeach
    endif
  endforeach
endforeach

install_emptydir(theme_dir / 'symbolic/apps')

pkgc.generate(
  name : 'default-icon-theme',
  description : 'The default fallback theme used by XDG icon-theme implementations.',
  dataonly : true,
)
