# Versioning

## Introduction

Until version [2.44] our installed files were not versioned,
so only one version of xkeyboard-config could be installed at a time.

We plan to introduce breaking changes to the rules and keymap file
formats to fix long-standing issues. To ensure backward compatibility,
we are switching to versioned install directories and files, in order
to enable multiple versions of xkeyboard-config to be installed in
parallel.

[2.44]: ChangeLog.md#:~:text=xkeyboard-config%202.44


## Version scheme

Since we do not expect the formats to evolve frequently, the version
scheme is simple:
- The file format version uses a single number and increments by 1.
- The *major* version of the xkeyboard-config project denotes the
  file format version of *both* rules and keymap files.
- The version scheme starts at the current major version: **2**.
- The version defines a range of XKB compilers that it is compatible
  with.
- Everytime we introduce a breaking change in the file formats, we bump
  the major version of xkeyboard-config and define the corresponding
  supported XKB compilers range.


## Policy

TODO: This a work in progress!

<dl>
<dt>What denotes the dataset formats?</dt>
<dd>

A xkeyboard-config format is defined by the range of Xorg xkbcomp and libxkbcommon
versions that can compile it.

So whenever we start to use a feature that does not compile with an older xkbcommon, we have to bump the format version.
</dd>
<dt>When do we introduce a new format?</dt>
<dd>

Shiny new syntax will not justify a format bump, only features that actually:
- improve xkeyboard-config maintenance
- improve end-users use cases
- fix bugs or strong limitations
</dd>
<dt>How long do we support a format?</dt>
<dd>

- v1: Until we drop support for X11 (not planned yet)
</dd>
</dl>


## Supported XKB compilers

<table>
<thead>
<tr>
<th rowspan="2">XKB compiler</th>
<th>xkeyboard-config format version</th>
</tr>
<tr>
<th>2</th>
</tr>
</thead>
<tbody>
<tr>
<th>

[X11 xkbcomp][xkbcomp]
</th>
<td>

all versions
</td>
</tr>
<tr>
<th>

[libxkbcommon]
</th>
<td>

all versions, using [`XKB_KEYMAP_FORMAT_TEXT_V1`](https://xkbcommon.org/doc/current/group__keymap.html)
</td>
</tr>
</tbody>
</table>

[xkbcomp]: https://gitlab.freedesktop.org/xorg/app/xkbcomp
[libxkbcommon]: https://xkbcommon.org/
