#!/usr/bin/env python3
"""Link complete native Pango and its Cairo/GIO/font/text dependencies."""
import argparse
import json
from pathlib import Path
import re

from link_cairo import link_stack,metadata_for as cairo_metadata
from build_pango import TEXT_RELEASES
from build_dependencies import digest


def metadata_for(root,names):
    additions={"libgio-2.0","libfribidi","libthai","libdatrie","libpango-1.0","libpangoft2-1.0","libpangocairo-1.0"}
    metadata=cairo_metadata(root,set(names)-additions)
    metadata["libgio-2.0"]=dict(metadata["libglib-2.0"])
    base=root/"src/Applications/text"
    for name in ("libthai","libdatrie"):
        source=base/name/TEXT_RELEASES[name]/"configure.ac"
        text=source.read_text()
        current,revision,age=[int(re.search(r"^LT_"+key+r"=(\d+)$",text,re.M)[1]) for key in ("CURRENT","REVISION","AGE")]
        if age>current:raise ValueError("invalid text library interface age")
        metadata[name]={"major":str(current-age),"compatibility":f"{current+1}.0.0","current":f"{current+1}.{revision}.0",
                        "source":str(source.relative_to(root)),"source_sha256":digest(source)}
    source=base/"fribidi"/TEXT_RELEASES["fribidi"]/"configure.ac"
    text=source.read_text()
    # FriBidi's Meson build omits darwin_versions. Its canonical libtool
    # release recipe supplies the actual Darwin current/revision/age mapping.
    values={key:int(re.search(r"m4_define\(fribidi_"+key+r",\s*(\d+)\)",text)[1])
            for key in ("interface_version","interface_age","binary_age")}
    current,revision,age=[values[key] for key in ("interface_version","interface_age","binary_age")]
    metadata["libfribidi"]={"major":str(current-age),"compatibility":f"{current+1}.0.0","current":f"{current+1}.{revision}.0",
                            "source":str(source.relative_to(root)),"source_sha256":digest(source)}
    source=base/"pango"/TEXT_RELEASES["pango"]/"meson.build"
    text=source.read_text()
    if "pango_osxversion = [osx_current, '@0@.@1@.0'.format(osx_current, pango_interface_age)]" not in text:
        raise ValueError("Pango Darwin version convention changed")
    _,minor,micro=map(int,TEXT_RELEASES["pango"].split("."))
    interface_age=0 if minor%2 else micro
    current=minor*100+micro-interface_age+1
    for name in ("libpango-1.0","libpangoft2-1.0","libpangocairo-1.0"):
        metadata[name]={"major":"0","compatibility":f"{current}.0.0","current":f"{current}.{interface_age}.0",
                        "source":str(source.relative_to(root)),"source_sha256":digest(source)}
    if set(names)!=set(metadata):raise ValueError("unexpected native Pango archive inventory")
    return metadata


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("root","libraries","system","dyld","output","report"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    link_stack(parser.parse_args(),metadata_for)


if __name__=="__main__":main()
