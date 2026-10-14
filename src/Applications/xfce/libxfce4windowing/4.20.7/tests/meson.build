add_test_setup(
  'default',
  exclude_suites: 'gui',
  is_default: true,
)
add_test_setup(
  'full',
)

test_bins = [
]
test_gui_bins = [
  'xfw-enum-monitors',
  'xfw-enum-windows',
  'xfw-enum-workspaces',
  'xfw-monitor-offon',
]

foreach bin : test_bins + test_gui_bins
  e = executable(
    bin,
    sources: [
      '@0@.c'.format(bin),
    ],
    include_directories: [
      include_directories('..'),
    ],
    dependencies: [
      gtk,
    ],
    link_with: [
      libxfce4windowing,
    ],
    install: false,
  )

  if bin in test_gui_bins
    test(bin, e, suite: 'gui')
  else
    test(bin, e)
  endif
endforeach
