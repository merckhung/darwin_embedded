project('xkeyboard-config',
        version: '2.48',
        license: 'MIT/Expat',
        meson_version: '>= 0.61.0')

xkc_version = meson.project_version().split('.')
xkc_version_major = xkc_version[0]
dir_data         = join_paths(get_option('prefix'), get_option('datadir'))
dir_xkb_base     = join_paths(dir_data, f'xkeyboard-config-@xkc_version_major@')
dir_xkb_base_x11 = join_paths(dir_data, 'X11', 'xkb')
dir_xkb_rules    = join_paths(dir_xkb_base, 'rules')
dir_man7         = join_paths(get_option('prefix'), get_option('mandir'), 'man7')
dir_pkgconfig    = join_paths(dir_data, 'pkgconfig')

i18n = import('i18n')

pkgconfig = import('pkgconfig')
pkgconfig_file = f'xkeyboard-config-@xkc_version_major@'
pkgconfig.generate(
    filebase: pkgconfig_file,
    name: 'XKeyboardConfig',
    description: 'X Keyboard configuration data',
    version: meson.project_version(),
    variables: [
        f'datadir=@dir_data@',
        # Canonical XKB root: /usr/share/xkeyboard-config-N/
        f'xkb_root=@dir_xkb_base@',
        # Legacy X11 XKB root, for backward-compatibility: /usr/share/X11/xkb
        f'xkb_base=@dir_xkb_base_x11@',
    ],
    install_dir: dir_pkgconfig,
    dataonly: true,
)
# Symbolic link for legacy unversioned file
if xkc_version_major != '2'
    error('This symlink is intended only while there is a single database format')
endif
install_symlink(
    'xkeyboard-config.pc',
    install_dir: dir_pkgconfig,
    pointing_to: f'@pkgconfig_file@.pc',
)

# KcG.T only need to be installed as-is
foreach dir: ['compat', 'geometry', 'keycodes', 'types']
    install_subdir(dir,
                   exclude_files: ['custom'],
                   install_dir: dir_xkb_base)
endforeach

# Rules and symbols are a bit more complicated
subdir('rules')
subdir('symbols')

# man page
xsltproc = find_program('xsltproc', required: false)
if xsltproc.found()
  man_substs = configuration_data()
  man_substs.set('xkb_base', dir_xkb_base)
  # emulating what the macros do for vendorversion, hardcoding the man
  # suffixes
  man_substs.set('vendorversion', '"@0@ @1@" "X Version 11"'.format(meson.project_name(), meson.project_version()))
  xsl = configure_file(input: 'man/man.xsl',
                       output: 'man.xsl',
                       configuration: man_substs,
                       install: false)
  # evdev_ruleset is set by rules/meson.build
  man_page_file = f'xkeyboard-config-@xkc_version_major@.7'
  manpage = custom_target('man page',
                          output: man_page_file,
                          build_by_default: true,
                          command: [xsltproc, '-nonet', xsl, evdev_ruleset],
                          capture: true,
                          install:true,
                          install_dir: dir_man7)
  # Symlink for legacy unversioned man page
  if xkc_version_major != '2'
      error('This symlink is intended only while there is a single database format')
  endif
  install_symlink(
      'xkeyboard-config.7',
      install_dir: dir_man7,
      pointing_to: man_page_file,
  )

  verify_group_names = find_program('tests/verify-group-names.sh')
  test('verify group names', verify_group_names)
endif

pymod = import('python')
python = pymod.find_installation('python3',
                                 modules: ['pytest'],
                                 required: false)

# Latin layout list
# Needed e.g. for a distribution installer, in order to check if user can
# input required Latin characters and if not add automatically a US layout.
if get_option('non-latin-layouts-list')
    python = pymod.find_installation('python3',
                                     modules: ['yaml'],
                                     required: true)
    xkbcli = find_program('xkbcli', required: true)
    pyregistry = find_program('scripts/registry.py', required: true)

    # Both rules sets are expected to provide the same results, but are
    # nevertheless generated for completeness.
    foreach ruleset: ['base', 'evdev']
        custom_target('non-latin-layouts-list-@0@'.format(ruleset),
                      build_by_default: true,
                      command: [pyregistry,
                                '--xkb-root', meson.project_build_root(),
                                '--xkb-root', meson.project_source_root(),
                                '--rules', ruleset,
                                'layouts',
                                '--non-latin',
                                '--csv', '@OUTPUT@'],
                      output: 'non-latin-layouts-@0@.csv'.format(ruleset),
                      depends: rules_files,
                      depend_files: registry_files,
                      install: false)
    endforeach
endif

# Legacy X11 symlink
fs = import('fs')
install_symlink(
    fs.name(dir_xkb_base_x11),
    install_dir: fs.parent(dir_xkb_base_x11),
    pointing_to: dir_xkb_base,
)

# pytest suite
pytest = find_program('pytest-3', 'pytest', required: false)
enable_pytest = python.found() and pytest.found()
if enable_pytest
    pytest_args = ['--verbose', '--log-level=DEBUG']
    if get_option('compat-rules')
        pytest_args += [
            'tests',
            '--layout-compat-config', layout_mappings,
            '--layout-compat-config', variant_mappings,
        ]
    endif
    # use pytest xdist if available, it really speeds up the tests cases
    optional_python_modules = ['xdist']
    if pymod.find_installation('python3', modules: optional_python_modules, required: false).found()
        pytest_args += ['-n', 'auto']
    endif

    test('pytest', pytest,
         args: pytest_args,
         env: {
            'XKB_CONFIG_ROOT': meson.project_source_root(),
            'XKB_CONFIG_EXTRA_PATH': meson.project_build_root(),
         },
         depends: rules_files,
         workdir: meson.project_source_root(),
         timeout: 60)
endif

subdir('po')
