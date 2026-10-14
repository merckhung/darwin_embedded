# Files that are a straight install
install_data('README', 'xkb.dtd', 'xfree98',
             install_dir: dir_xkb_rules)

base_xml = files('base.xml')
base_extras_xml = files('base.extras.xml')

MINIMUM_PYTHON_VERSION = '3.11'
pymod = import('python')
python = pymod.find_installation('python3')
if python.language_version().version_compare('<@0@'.format(MINIMUM_PYTHON_VERSION))
    error('Minimum required Python version: @0@, but got: @1@'.format(
        MINIMUM_PYTHON_VERSION,
        python.language_version()
    ))
endif
xml2lst = find_program('xml2lst.pl')
rules_template = files('rules.in')
rules_files = []
registry_files = []
foreach ruleset: ['base', 'evdev']
    # First: generate the "evdev" and "base" rules files
    cmd = [
        'python3',
        '-m',
        'rules.generator',
        'rules',
        '--ruleset',
        ruleset,
        '--version',
        'v2',
        '--output',
        meson.current_build_dir() / ruleset
    ]
    if get_option('compat-rules')
        cmd += ['--compat']
    endif
    cmd += rules_template
    rules_files += custom_target(f'rules-@ruleset@',
                                 build_by_default: true,
                                 command: cmd,
                                 depend_files: rules_template,
                                 env: { 'PYTHONPATH': meson.project_source_root() },
                                 output: ruleset,
                                 install: true,
                                 install_dir: dir_xkb_rules)

    # Second: generate the xml files, simply copied from the base*.xml files
    ruleset_xml = configure_file(output: f'@ruleset@.xml',
                                 input: base_xml,
                                 copy: true,
                                 install: true,
                                 install_dir: dir_xkb_rules)
    registry_files += ruleset_xml
    # This is used by the man page's meson.build
    if ruleset == 'evdev'
      evdev_ruleset = ruleset_xml
    endif
    registry_files += configure_file(output: f'@ruleset@.extras.xml',
                                     input: base_extras_xml,
                                     copy: true,
                                     install: true,
                                     install_dir: dir_xkb_rules)

    # Third: generate the evdev.lst and base.lst files
    lst_file = f'@ruleset@.lst'
    custom_target(lst_file,
                  build_by_default: true,
                  command: [xml2lst, ruleset_xml],
                  capture: true,
                  output: lst_file,
                  install: true,
                  install_dir: dir_xkb_rules)
endforeach

if get_option('xorg-rules-symlinks')
  foreach suffix: ['', '.lst', '.xml']
    if meson.version().version_compare('>= 0.61')
      install_symlink('xorg' + suffix,
                      pointing_to: 'base' + suffix,
                      install_dir: dir_xkb_rules)
    else
      meson.add_install_script('sh', '-c',
        'ln -s base@0@ $DESTDIR@1@/xorg@0@'.format(suffix, dir_xkb_rules))
    endif
  endforeach
endif

# Copy the DTD to the build directory, the man page generation expects it in
# the same directory as the input XML file.
configure_file(output: 'xkb.dtd',
               input: 'xkb.dtd',
               copy: true,
               install: false)
