# Fontconfig Test JSON Format Documentation

This document describes the JSON format used by the `test-conf` test harness for testing fontconfig behavior.

## Overview

The test JSON format allows you to define:
- A mock font database with specific font properties
- Environment variables and configuration files to load
- Test queries and expected results

## Top-Level Structure

```json
{
  "env": { ... },
  "fonts": [ ... ],
  "appfonts": [ ... ],
  "filter": { ... },
  "load_xml": [ ... ],
  "tests": [ ... ]
}
```

### Fields

#### `env` (optional)
Type: Object

Environment variables to set before running tests. Special key `"locale"` calls `setlocale()` instead of `setenv()`.

**Example:**
```json
{
  "env": {
    "locale": "de_DE.UTF-8",
    "FC_DEBUG": "1"
  }
}
```

#### `fonts` (required)
Type: Array of Pattern objects

Defines the system font database. Each entry represents a font with its properties.

**Example:**
```json
{
  "fonts": [
    {
      "family": "Noto Sans",
      "style": "Regular",
      "file": "/path/to/NotoSans.ttf",
      "fontversion": 1
    }
  ]
}
```

#### `appfonts` (optional)
Type: Array of Pattern objects

Defines application-level fonts, which can take precedence over system fonts depending on configuration.

**Example:**
```json
{
  "appfonts": [
    {
      "family": "Custom Font",
      "file": "/app/path/to/CustomFont.ttf",
      "fontversion": 2
    }
  ]
}
```

#### `filter` (optional)
Type: Pattern object

A pattern used to filter which fonts from the `fonts` array are actually included in the font set. Only fonts matching all properties in the filter pattern are included.

**Example:**
```json
{
  "filter": {
    "fontwrapper": "SFNT"
  }
}
```

#### `load_xml` (optional)
Type: Array of strings

List of fontconfig XML configuration files to load and apply. Use `%test%` as a placeholder for the test directory (defined by SRCDIR).

**Example:**
```json
{
  "load_xml": [
    "conf.d/48-guessfamily.conf",
    "conf.d/49-sansserif.conf",
    "%test%/test-custom.conf"
  ]
}
```

#### `tests` (required)
Type: Array of Test objects

Test cases to execute against the configured fontconfig instance.

## Pattern Object Format

Patterns are used in `fonts`, `appfonts`, `filter`, test `query`, and test `result` objects. A pattern is a JSON object where keys are fontconfig property names and values specify the property values.

### Value Types

#### String Values

Simple string properties:
```json
{
  "family": "Noto Sans",
  "style": "Regular",
  "file": "/path/to/font.ttf"
}
```

For properties that expect constants (like `weight`, `slant`, `width`), you can use constant names:
```json
{
  "weight": "bold",
  "slant": "italic",
  "width": "condensed"
}
```

Special string value:
- `"DontCare"`: Represents the FcDontCare boolean value

#### Numeric Values

Integer or double values:
```json
{
  "fontversion": 1,
  "size": 12.5,
  "weight": 200,
  "index": 0
}
```

#### Boolean Values

```json
{
  "scalable": true,
  "variable": false,
  "namedinstance": false,
  "embolden": true
}
```

#### Null Values

```json
{
  "embolden": null
}
```

#### Array Values

Arrays are interpreted based on the property type and array content:

**String Arrays** (for properties like `family`, `style`):
```json
{
  "family": ["Noto Sans", "sans-serif"],
  "style": ["Regular", "Normal"]
}
```

**Charset Arrays** (for `charset` property):
Each element is a single UTF-8 character (codepoint):
```json
{
  "charset": ["a", "b", "c", "あ", "😀"]
}
```

**LangSet Arrays** (for `lang` property):
Each element is a language code:
```json
{
  "lang": ["en", "de", "ja"]
}
```

**Integer Arrays** (for properties like `weight` when multiple values needed):
```json
{
  "weight": [80, 100, 200]
}
```
Can also use constant names:
```json
{
  "weight": ["light", "medium", "bold"]
}
```

**Double Arrays** (for properties expecting multiple double values):
```json
{
  "dpi": [96.0, 120.0]
}
```

**Range Arrays** (2-element numeric array):
```json
{
  "size": [8.0, 48.0]
}
```

**Matrix Arrays** (4-element numeric array `[xx, xy, yx, yy]`):
```json
{
  "matrix": [1.0, 0.0, 0.0, 1.0]
}
```

## Test Object Format

```json
{
  "method": "match",
  "config": { ... },
  "query": { ... },
  "result": { ... },
  "result_fs": [ ... ],
  "$comment": "Optional comment"
}
```

### Fields

#### `method` (required)
Type: String

The test method to use. Valid values:
- `"match"`: Test `FcFontMatch()` - find the best matching font
- `"list"`: Test `FcFontList()` - list all fonts matching a pattern
- `"sort"`: Test `FcFontSort()` with trimming - sorted list of matching fonts (trimmed)
- `"sort_all"`: Test `FcFontSort()` without trimming - sorted list of all matching fonts
- `"pattern"`: Test `FcConfigSubstitute()` - pattern substitution without font matching

#### `config` (optional)
Type: Object

Per-test configuration options applied before running the test.

**Supported options:**
- `"prefer_app_font"`: Boolean - whether to prefer application fonts over system fonts

**Example:**
```json
{
  "config": {
    "prefer_app_font": true
  }
}
```

#### `query` (required)
Type: Pattern object

The input pattern for the query.

**Example:**
```json
{
  "query": {
    "family": "Noto Sans",
    "weight": "bold",
    "size": 12.0
  }
}
```

#### `result` (required for `match` and `pattern` methods)
Type: Pattern object

The expected result pattern. The test verifies that all properties specified in this pattern match the corresponding properties in the actual result.

**Example:**
```json
{
  "result": {
    "family": "Noto Sans",
    "file": "/path/to/NotoSans-Bold.ttf",
    "weight": 200
  }
}
```

#### `result_fs` (required for `list`, `sort`, and `sort_all` methods)
Type: Array of Pattern objects

The expected result font set. The test verifies:
1. The number of results matches
2. Each result pattern matches the expected pattern at the same index

**Example:**
```json
{
  "result_fs": [
    {
      "family": "Noto Sans",
      "file": "/path/to/NotoSans-Regular.ttf"
    },
    {
      "family": "Noto Sans",
      "file": "/path/to/NotoSans-Bold.ttf"
    }
  ]
}
```

#### `$comment` (optional)
Type: String

A comment field that is ignored by the test runner. Use for documentation purposes.

**Example:**
```json
{
  "$comment": "This test verifies that bold synthesis works correctly"
}
```

## Common Pattern Properties

Here are commonly used fontconfig properties you can use in patterns:

### Font Identification
- `family`: String or Array - Font family name(s)
- `style`: String or Array - Font style name(s)
- `fullname`: String - Full font name
- `file`: String - Font file path
- `index`: Integer - Font index in collection

### Font Attributes
- `weight`: Integer or Constant - Font weight (0-215, or "thin", "light", "medium", "bold", "black", etc.)
- `slant`: Integer or Constant - Font slant ("roman", "italic", "oblique")
- `width`: Integer or Constant - Font width ("condensed", "normal", "expanded", etc.)
- `size`: Double or Range - Font size in points
- `pixelsize`: Double or Range - Font size in pixels
- `fontversion`: Integer - Font version number

### Font Capabilities
- `scalable`: Boolean - Whether font is scalable
- `outline`: Boolean - Whether font has outlines
- `color`: Boolean - Whether font has color glyphs
- `variable`: Boolean - Whether font is a variable font
- `namedinstance`: Boolean - Whether font is a named instance

### Character Support
- `charset`: Array of characters - Supported characters
- `lang`: String or Array - Supported language(s)

### Typography
- `spacing`: Integer or Constant - Character spacing ("proportional", "mono", "charcell")
- `fontformat`: String - Font format ("TrueType", "Type 1", "CFF", etc.)
- `fontwrapper`: String - Font wrapper format ("SFNT", "CFF", "WOFF", etc.)

### Rendering
- `antialias`: Boolean - Whether to antialias
- `hinting`: Boolean - Whether to hint
- `hintstyle`: Integer or Constant - Hint style ("none", "slight", "medium", "full")
- `rgba`: Integer or Constant - Subpixel order ("none", "rgb", "bgr", "vrgb", "vbgr")
- `embolden`: Boolean or Null - Whether to embolden (synthetic bold)
- `matrix`: Array[4] - Transformation matrix

### Advanced
- `dpi`: Double - DPI setting
- `genericfamily`: Integer or Constant - Generic family ("serif", "sans-serif", "monospace", etc.)
- `fontvariations`: String - Font variation settings (e.g., "wght=400,wdth=100")

## Complete Example

```json
{
  "env": {
    "locale": "en_US.UTF-8"
  },
  "fonts": [
    {
      "family": "Noto Sans",
      "style": "Regular",
      "file": "/path/to/NotoSans-Regular.ttf",
      "weight": 80,
      "slant": "roman",
      "fontversion": 1,
      "scalable": true,
      "outline": true
    },
    {
      "family": "Noto Sans",
      "style": "Bold",
      "file": "/path/to/NotoSans-Bold.ttf",
      "weight": 200,
      "slant": "roman",
      "fontversion": 1,
      "scalable": true,
      "outline": true
    }
  ],
  "load_xml": [
    "conf.d/10-autohint.conf",
    "conf.d/10-hinting-slight.conf"
  ],
  "tests": [
    {
      "$comment": "Test that bold weight request matches bold font",
      "method": "match",
      "query": {
        "family": "Noto Sans",
        "weight": "bold"
      },
      "result": {
        "family": "Noto Sans",
        "file": "/path/to/NotoSans-Bold.ttf",
        "weight": 200
      }
    },
    {
      "$comment": "Test that listing returns all Noto Sans variants",
      "method": "list",
      "query": {
        "family": "Noto Sans"
      },
      "result_fs": [
        {
          "family": "Noto Sans",
          "file": "/path/to/NotoSans-Regular.ttf"
        },
        {
          "family": "Noto Sans",
          "file": "/path/to/NotoSans-Bold.ttf"
        }
      ]
    }
  ]
}
```

## Usage

To run a test:

```bash
test-conf <config-file> <test-json-file>
```

Where:
- `<config-file>`: Base fontconfig configuration file (XML format)
- `<test-json-file>`: Test scenario file (JSON format described in this document)

The test harness will:
1. Create a fontconfig configuration instance
2. Load the base config file
3. Set environment variables from `env`
4. Build the mock font database from `fonts` and `appfonts`
5. Apply the `filter` if specified
6. Load additional config files from `load_xml`
7. Execute each test in `tests` and verify results

Exit code is 0 if all tests pass, non-zero otherwise.
