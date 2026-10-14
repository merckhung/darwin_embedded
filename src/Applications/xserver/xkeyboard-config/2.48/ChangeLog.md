xkeyboard-config [2.48] - 2026-06-21
====================================

[2.48]: https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/tree/xkeyboard-config-2.48
================================================================================================================================================================================
Models
------

No significant changes.


Layouts
-------

Breaking changes
~~~~~~~~~~~~~~~~

- Spanish layout: move `dead_doubleacute` and `dead_caron` to easier to remember positions:
  - `dead_doubleacute` is moved to same key as `dead_acute` and `dead_diaeresis`, replacing `dead_caron`.
  - `dead_caron` is moved to same key as `dead_circumflex`, replacing `dead_abovering`.
  - `dead_abovering` is moved to `Ntilde`, where `dead_doubleacute` used to be.


New
~~~

- - `pl`: Added Colemak-DH ortholinear variant

     Contributed by Aria Węgrzyn ([#pl-colemak-dh-ortho](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/pl-colemak-dh-ortho))
- `ca(multix)`:
  - Added `oneeighth` (U+215B) to `<AE02>`
  - Duplicated `currency` (U+00A4) on `<AE04>`
  - Added `dstroke` (U+0111) to `<AC04>`

  Contributed by Alexandre Bouvier ([#563](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/563))
- Added `br(thinkpad_nodeadkeys)`: Portuguese (Brazil, IBM/Lenovo ThinkPad, no dead keys). ([#868](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/868))


Options
-------

No significant changes.


Miscellaneous
-------------

Breaking changes
~~~~~~~~~~~~~~~~

- Made key type `KEYPAD` independent of `Shift`, so that `Shift` can be used in
  keyboard shortcuts with numpad keys, with and without `NumLock`.

  This removes the counter-intuitive behavior of `Shift + NumLock` mapping to the
  arrow/edition keys on the numpad.
- numpad  key types: Changed `FOUR_LEVEL_KEYPAD` and `FOUR_LEVEL_MIXED_KEYPAD`
  so that they are identical and enable to use `Shift` in keyboard shortcuts
  with *both* arrows/edition keys and numbers:

  - `Shift` maps to level 1, usually corresponding to numpad `KP_`
    arrows/editing keysyms variants);
  - `Shift` is preserved.
  - No “`Shift` cancels `NumLock`” behavior.

  All numpad key types have now the same consistent behavior with `Shift`
  and `NumLock`.


New
~~~

- inet: Added mapping to the following new keysyms:
  - `XF86ElectronicPrivacyScreenOn`
  - `XF86ElectronicPrivacyScreenOff`
  - `XF86ActionOnSelection`
  - `XF86ContextualInsert`
  - `XF86ContextualQuery`

  Relevant upstream merge requests: [xorgproto-109], [xorgproto-112]

  [xorgproto-109]: https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/merge_requests/109
  [xorgproto-112]: https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/merge_requests/112


Fixes
~~~~~

- Fixed key types missing or erroneous `map` and `preserve` entries:
  - `extra`: `FOUR_LEVEL_X`
  - `numpad(shift3)`: `FOUR_LEVEL_MIXED_KEYPAD`
  - `pc`: `PC_SHIFT_SUPER_LEVEL2` and `CTRL+ALT`


Build system
------------

Breaking changes
~~~~~~~~~~~~~~~~

- Bumped the minimum required Python version to 3.11 ([#11](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/11))


xkeyboard-config [2.47] - 2026-02-23
====================================

[2.47]: https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/tree/xkeyboard-config-2.47

# Layouts

## New

- Added the Slavistic Phonetic Alphabet variant for Polish

# Miscellaneous

## Breaking changes

- Made <ZEHA> behave like <FK24>

  On Linux Kernel before v6.17, the scancode for F24 was bound to the otherwise
  unused <ZEHA> keycode. v6.17 fixed this. To have a consistent behaviour across
  kernel versions, make both <ZEHA> and <FK24> behave the same.

## New

- Added keycodes from recent Linux kernels:
  - `<I455>` for `KEY_LINK_PHONE`
  - `<I709>` for `KEY_PERFORMANCE`
- inet: Added mapping to the following new keysyms:
  - `XF86LinkPhone`
  - `XF86Fn_F1`
  - `XF86Fn_F2`
  - `XF86Fn_F3`
  - `XF86Fn_F4`
  - `XF86Fn_F5`
  - `XF86Fn_F6`
  - `XF86Fn_F7`
  - `XF86Fn_F8`
  - `XF86Fn_F9`
  - `XF86Fn_F10`
  - `XF86Fn_F11`
  - `XF86Fn_F12`
  - `XF86Fn_1`
  - `XF86Fn_2`
  - `XF86Fn_D`
  - `XF86Fn_E`
  - `XF86Fn_F`
  - `XF86Fn_S`
  - `XF86Fn_B`
  - `XF86PerformanceMode`
  - `XF86AudioBassBoost`

  Relevant upstream merge requests: [xorgproto-102] and [xorgproto-103].

  [xorgproto-102]: https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/merge_requests/102
  [xorgproto-103]: https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/merge_requests/103
- inet: Mapped `F19` for the rare occasion that it exists e.g. on custom keyboards.
- inet: Mapped `F24`, which has a special alternative function as pressing the
  touchpad toggle key on some notebooks produces the key sequence `Super + Control + F24`.


xkeyboard-config [2.46] - 2025-09-30
====================================

[2.46]: https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/tree/xkeyboard-config-2.46

# Layouts

## New

- Added Manoonchai layout for Thai as documented in [Github repository](https://github.com/manoonchai/manoonchai?tab=readme-ov-file).

  Contributed by Pawat Nakpiphatkulr
  ([!820](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/820))
- Added ISO/international variants of ANSI and Dvorak US Macintosh layouts.

  Contributed by Katalin Rebhan
  ([!829](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/829))
- Added the Ukrainian (Windows Enhanced) keyboard layout variant (winkeysenhanced) which matches the Windows 11 default.

  Contributed by Alex Dowson
  ([!838](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/838))
- Add `pk(pak_urdu_phonetic)` variant
  This layout provides phonetic typing for Urdu, mapping English phonetic equivalents to Urdu script.
  It is a port of the widely-used Windows-only “Pak Urdu Installer” by mBilalm.
  Includes comprehensive character coverage with over 160 custom key mappings.
  Adds support for full Urdu typography including diacritics, punctuation, and special symbols.
  Introduces `U+FDFB` (ﷻ) mapped to `AltGr+Shift+X`.

  Contributed by Nashit Ahmed Barq
  ([!839](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/839))
- Added an alternative variant for Gothic in the Ancient layout.

  Contributed by Garcez
  ([!845](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/845))

## Fixes

- Fixed a regression in `us(mac)` layout resulting in the grave/tilde key inverted
  with the section key.

  Contributed by Katalin Rebhan
  ([!829](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/829),
  [#534](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/534))


# Options

## New

- Added `hyper:mod2` compatibility option, to maps the virtual modifier `Hyper`
  to `Mod2`; *conflicts with `NumLock`*.

  Use this option if using `Hyper`, `Super` *and* `LevelFive`, e.g. for layouts
  with 5+ levels. The option `numpad:mac` should be activated as well, in order to
  enable the numpad.

## Fixes

- Fixed `shift:break_caps` resulting in rEVERSE cAPS if some keys were operated
  simultaneously with the `Shift` keys when trying to unlock `Caps`.
  ([#74](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/74))


# Miscellaneous

## New

- inet: Added mapping to the following new keysyms:
  - `XF86OK`
  - `XF86Select`
  - `XF86GoTo`
  - `XF86Clear`
  - `XF86Option`
  - `XF86Time`
  - `XF86VendorLogo`
  - `XF86MediaSelectProgramGuide`
  - `XF86NextFavorite`
  - `XF86MediaSelectProgramGuide`
  - `XF86MediaSelectHome`
  - `XF86MediaLanguageMenu`
  - `XF86MediaTitleMenu`
  - `XF86Subtitle`
  - `XF86AudioChannelMode`
  - `XF86MediaSelectPC`
  - `XF86MediaSelectTV`
  - `XF86MediaSelectCable`
  - `XF86MediaSelectVCR`
  - `XF86MediaSelectVCRPlus`
  - `XF86MediaSelectSatellite`
  - `XF86MediaSelectCD`
  - `XF86MediaSelectTape`
  - `XF86MediaSelectRadio`
  - `XF86MediaSelectTuner`
  - `XF86MediaPlayer`
  - `XF86MediaSelectTeletext`
  - `XF86MediaSelectAuxiliary`

  Relevant upstream merge request: [xorgproto-93].

  [xorgproto-93]: https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/merge_requests/93

## Fixes

- Fixed `CTRL+ALT` incomplete key type, which prevented using some key combinations:
  e.g. `Control+Backspace`.


# Build system

## Breaking changes

- Remove the build option `xkb-base`, which was ineffectual since the migration
  from autotools to meson in xkeyboard-config 2.35, published 3 years ago.
  ([#532](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/532))


xkeyboard-config [2.45] - 2025-06-08
====================================

[2.45]: https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/tree/xkeyboard-config-2.45

## Models

### Fixes

- Norwegian Macintosh layout: Fixed the `apostrophe` key to actually output
  `apostrophe` instead of `bar`.

  Contributed by Håvard Bærug
  ([!801](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/801))


## Layouts

### Breaking changes

- Deleted obsolete `jp(kana86)` layout.
  ([#502](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/502))

### New

- Added Colemak variant for Swedish: `se(colemak)`.

  Contributed by satricus
  ([!761](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/761))
- French (AZERTY, AFNOR): Implemented the “European character” dead key on `AltGr+H`,
  as documented at the [offical web page](https://norme-azerty.fr/img/EU_level.png).
  Also fixed the missing upper Theta Θ on `Q`.
- `ru(typo)`: Added bar symbol on the `<BKSL>` key.

  Contributed by Boolat Kamalov
  ([!814](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/814))

### Fixes

- `us(colemak_dh)`: Fix the CapsLock remapping being difficult to override.
  ([#490](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/490))
- Japanese layouts: Fix Eisu toggle triggering CapsLock when the layout is not in
  the first position. ([#512](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/512))
- Apple: fix ISO keyboards International English layout (`<TLDE>`, `<LSGT>` keys permutation).

  Contributed by Andrey Butirsky
  ([!793](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/793))
- Faroese `fo`: use comma as the decimal separator for the numpad.

  Contributed by Ragnar Kruse
  ([!815](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/815))


## Options

### New

- Added two compatibility options related to `Hyper`:
  - `hyper:mod3`: maps the virtual modifier `Hyper` to `Mod3`; *conflicts with `LevelFive`*.
    Loaded by default when using any option with `Hyper`.
  - `hyper:mod4`: maps the virtual modifier `Hyper` to `Mod3`; *conflicts with `Super`*.
    Use this option if using both `Hyper` *and* `LevelFive`, e.g. for layouts with 5+ levels.

### Fixes

- `shift:breaks_caps`: Fix missing `Shift_{L,R}` keysyms that prevented
  keyboard shortcuts to work correctly in some setups.


## Miscellaneous

### Breaking changes

- Added `KEY_ZENKAKUHANKAKU` mapping for touchpad toggle.

  Pressing the touchpad toggle key on some notebooks produces the key sequence
  `Super + Control + KEY_ZENKAKUHANKAKU`. Actual Japanese Hankaku/Zenkaku keys
  however do not use this keycode, but the <TLDE> one instead.

  Contributed by Werner Sembach
  ([!810](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/810))
- The default mapping of the virtual modifier `Hyper` to `Mod3` is now deactivated
  by default, because it conflicts with `LevelFive`, which is used in various layouts
  with 5+ levels.

  It is now enabled only when using any option binding `Hyper` keysyms or when
  using the new option `hyper:mod3`. However, if one one want to use `LevelFive`
  and `Hyper` simultaneously (e.g. for layouts with 5+ levels), then the new
  alternative option `hyper:mod4` should be used instead.


## Build system

### Breaking changes

- Switched to versioned install directories and files, to enable installing
  multiple versions of xkeyboard-config to be installed in parallel.

  - Moved the keyboard keymap data to a namespace dedicated to xkeyboard-config:
    `<prefix>/<datadir>/xkeyboard-config-2`.
  - Created symbolic link to maintain backward-compatibility with the X11 namespace:
    `<prefix>/<datadir>/X11/xkb` → `<prefix>/<datadir>/xkeyboard-config-2`.
  - Renamed `pkg-config`, translation and manual files to include a version:
    - `<prefix>/<datadir>/pkgconfig/xkeyboard-config-2.pc`
    - `<prefix>/<mandir>/man7/xkeyboard-config-2.7`
    - `<prefix>/<localedir>/**/xkeyboard-config-2.mo`
  - Created unversioned symbolic links to the previous files for backward-compatibility:
    - `<prefix>/<datadir>/pkgconfig/`: `xkeyboard-config.pc` → `xkeyboard-config-2.pc`
    - `<prefix>/<mandir>/man7/`: `xkeyboard-config.7` → `xkeyboard-config-2.7`
    - `<prefix>/<localedir>/**/`: `xkeyboard-config.mo` → `xkeyboard-config-2.mo`

  See [our versioning documentation](VERSIONING.md) for further information.


xkeyboard-config [2.44] - 2025-02-05
====================================

[2.44]: https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/tree/xkeyboard-config-2.44

## Models

### Fixes

- Fixed rules for vendor-specific layouts, in particular for Sun and Apple
  Aluminium keyboards.
  ([#112](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/112))
- Apple: swap `<TLDE>` and `<LSGT>` keysyms for Russian layout.


## Layouts

### Breaking changes

- Breton: Fixed capitalization of “c’h” and “ch”
  [multigraphs](https://en.wikipedia.org/wiki/Multigraph_%28orthography%29) using
  CapsLock, as well as Greek letters. A few Greeks letters were moved to more
  intuitive places.
  ([#480.breton](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/480.breton))

### New

- Added Noted layout `de(noted)` for German.

  Contributed by Benjamin Drung
  ([!681](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/681))


## Options

### Breaking changes

- Disabled "Alt and Meta are on Alt" by default. The old behavior can be restored
  by enabling the `altwin:meta_alt` option.
  ([#488](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/488))


## Miscellaneous

### New

- inet: Added `XF86Assistant` as default mapping to the Super + Shift + F23
  keyboard combo, aka the "Copilot Key".
- inet: Added mapping to the following new keysyms:
  - `XF86RefreshRateToggle`
  - `XF86Accessibility`
  - `XF86DoNotDisturb`

  Relevant upstream merge request: [xorgproto-91].

  [xorgproto-91]: https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/merge_requests/91

### Fixes

- Fixed missing virtual modifiers declarations, in order to allow all sections to
  be used in isolation.


xkeyboard-config [2.43] - 2024-10-01
====================================

[2.43]: https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/tree/xkeyboard-config-2.43

## Models

### New

- Restore geometries for Brazilian ABNT2 (`abnt2`), Japanese 106 (`jp106`)
  and Korean 106 (`kr106`) models. ([#292](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/292))

### Fixes

- geometry: Fixed label of `<LSGT>` key in Kinesis.

  Contributed by Arlen Kleinsasser


## Layouts

### Breaking changes

- `us(colemak_dh_wide_iso)`: Swapped `<AB06>` and `<AD12>` keys to match [specification](https://colemakmods.github.io/mod-dh/keyboards.html)

  Contributed by Callum Andrew ([#442](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/442))
- Updated `de(e1)` and `de(e2)`: implemented the changes made to these layouts in the latest revision of the specification, DIN 2137-1:2023-08; namely, some of the *group 2* symbols, that are accessed by first pressing Alt&nbsp;Gr+f, for keys `´`, `u`, `p`, `,`, and space bar were altered.

  Contributed by Jan Henning Klasen and Jakob Kramer. ([#745](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/745))
- `us(colemak_dh)`: Made `<CAPS>` key behave as Caps Lock by default, as shown in the [specification](https://colemakmods.github.io/mod-dh/keyboards.html).

### New

- Added Diktor layout `ru(diktor)` for Russian.

  Contributed by Hloya Nizhelska ([!712](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/712))
- Added the RuIntl keyboard layouts set `ru(ruintl_ru)`, `ru(ruintl_en)`.

  Contributed by Denis Kaliberov ([!752](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/merge_requests/752))
- Updated layout `us 3l` to include qwerty symbols and correct symbols for less than or equal and greater than or equal.

### Fixes

- rules: Fix broken layout compatibility rules, for symbols sections that where renamed or moved. ([#478](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/478))


## Options

### Breaking changes

- Map `Hyper` to `Mod3` by default to make `Super` and `Hyper` independent
  modifiers. ([#440](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/440))

### New

- Added `caps:return` to make the `Caps Lock` key an additional `Return` key. ([#121](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/121))
- Added `fkeys:basic_13-24`: define `F13-F24` keys with their corresponding function keysyms.

  Contributed by twistedturtle ([#306](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/306))
- Added `altwin:swap_ralt_rwin` to swap right `Alt` with right `Win`. ([#474](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/474))
- Added `caps:digits_row_independent_lock` option to lock digits on the digits
  row (Azerty layouts).

  Contributed by Alexandre Petit
- Added option `lv3:caps_switch_capslock_with_ctrl` to use Caps Lock as
  3rd-level chooser and Ctrl + Caps Lock as original Caps Lock action.

  Contributed by Helfried Wiesinger

### Fixes

- Added `caps:ctrl_shifted_capslock`: make `Caps Lock` an additional `Ctrl`
  and `Shift + Caps Lock` the regular `Caps Lock`.

  Contributed by Han-Miru Kim ([#447](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/447))


## Miscellaneous

### New

- Added `<I570>` keycode (`KEY_REFRESH_RATE_TOGGLE`).


## Build system

### New

- Add a new build option `non-latin-layouts-list` to generate lists of
  non-Latin layouts, e.g. layouts that cannot produce the basic A-Z Latin
  letters. This can be used e.g. in an OS installer to add automatically
  a default layout in such case. ([#120](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/120))

### Fixes

- Relaxed Python version requirement to support ≥ 3.9.
  Improved version detection and corresponding error messages. ([#465](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/465))


xkeyboard-config [2.42] - 2024-06-07
====================================

[2.42]: https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/tree/xkeyboard-config-2.42

## Models

### Breaking

- Removed the old Macs
- Removed the MacBook 78/79
- Removed the Intel Classmate
- Removed a few old Nokia devices


## Layouts

### Breaking change

- `dz`: Renamed `la` to `azerty-oss`.
- `br`: Removed the default `Scroll_Lock` mapping.

### New

- `ara(mac-phonetic)`: use new dead key `dead_hamza`.
- `dz`: Added `kab` to the language list.
- `fr`: Added Ergo‑L layout and variant (`ergol`, `ergol_iso`).
- `gr`: Added missing characters from `cp1253` and `varEpsilon`.
- `hu`: Added Old Hungarian layouts for users in SK
- symbols: Added grab and srvrkeys with a single section
- `ru`: Updated Rulemak to latest version.
- `ru(ruu)`: Added `Ukrainian_i` as `Cyrillic_i` alternative to the
   3rd level of `<AB05>`.
- `ua`: Enabled typing “g” with `AltGr`.

### Fixes

- `fr(oss)`: Updated behaviour of space key to match doc.
  [#439](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/439)
- `fr(bepo_afnor)`: Removed unnecessary include.
  [#448](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/448)


## Options

## New

- Added `eurosign:E`.
  [#444](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/issues/444)
- Added `caps:digits_row` for Azerty layouts.
- Added `scrolllock:mod3`.


Older versions
==============

Unfortunately there is no detailed changelog for older versions. Please use the
[git log](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/commits/master).
