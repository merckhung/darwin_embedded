/* Copyright (C) 2025 fontconfig Authors */
/* SPDX-License-Identifier: HPND */

#include "fcint.h"

static FcBool
FcConfigFileGenerateGenericAlias (FcConfig  *config,
                                  FcPattern *pat,
                                  FcPattern *font,
                                  FcStrBuf  *buf)
{
    FcChar8        *family = NULL, *file = NULL;
    const FcChar8  *gf, *s;
    int             generic_family;
    FcStrSet       *lset = NULL;
    FcStrList      *list = NULL;
    FcLangSet      *ls = NULL;
    FcBool          ret = FcTrue;
    FcBool          has_lang = FcFalse;
    FcPatternIter   iter;
    FcValueListPtr  vl;

    if (FcPatternObjectGetString (font, FC_FILE_OBJECT, 0, &file) != FcResultMatch) {
	fprintf (stderr, "Fontconfig warning: no file object in the font metadata\n");
	return FcFalse;
    }
    if (FcPatternObjectGetString (font, FC_FAMILY_OBJECT, 0, &family) != FcResultMatch) {
	fprintf (stderr, "Fontconfig warning: %s: no family object in the font metadata\n", file);
	return FcFalse;
    }

    FcPatternIterStart (pat, &iter);
    if (FcPatternFindObjectIter (pat, &iter, FC_GENERIC_FAMILY_OBJECT)) {
	vl = FcPatternIterGetValues (pat, &iter);
    } else {
	FcPatternIterStart (font, &iter);
	if (FcPatternFindObjectIter (font, &iter, FC_GENERIC_FAMILY_OBJECT)) {
	    vl = FcPatternIterGetValues (font, &iter);
	} else {
	    fprintf (stderr, "Fontconfig warning: %s: Unable to determine generic family from either of font nor pattern\n", file);
	    return FcFalse;
	}
    }

    FcPatternObjectGetLangSet (pat, FC_LANG_OBJECT, 0, &ls);
    if (ls) {
	lset = FcLangSetGetLangs (ls);
	if (!lset)
	    return FcFalse;
	if (lset->num > 0)
	    has_lang = FcTrue;
    }

    for (; vl; vl = FcValueListNext (vl)) {
	if (vl->value.type == FcTypeDouble)
	    generic_family = (int)vl->value.u.d;
	else if (vl->value.type == FcTypeInteger)
	    generic_family = vl->value.u.i;
	else
	    continue;

	gf = FcNameGetConstantNameFromObject (FC_GENERIC_FAMILY_OBJECT,
	                                      generic_family);
	if (!gf)
	    continue;

	if (has_lang) {
	    list = FcStrListCreate (lset);
	    if (!list) {
		ret = FcFalse;
		goto bail;
	    }
	    while ((s = FcStrListNext (list))) {
		if (!FcStrBufFormat (buf,
		                     "  <alias>\n"
		                     "    <family>%s</family>\n"
		                     "    <test name=\"lang\" compare=\"contains\">\n"
		                     "      <string>%s</string>\n"
		                     "    </test>\n"
		                     "    <prefer><family>%s</family></prefer>\n"
		                     "  </alias>\n\n",
		                     gf, s, family)) {
		    ret = FcFalse;
		    FcStrListDone (list);
		    goto bail;
		}
	    }
	    FcStrListDone (list);
	    list = NULL;
	} else {
	    if (!FcStrBufFormat (buf,
	                         "  <alias>\n"
	                         "    <family>%s</family>\n"
	                         "    <prefer><family>%s</family></prefer>\n"
	                         "  </alias>\n",
	                         gf, family)) {
		ret = FcFalse;
		goto bail;
	    }
	}
	if (!FcStrBufFormat (buf,
	                     "  <alias>\n"
	                     "    <family>%s</family>\n"
	                     "    <default><family>%s</family></default>\n"
	                     "  </alias>\n",
	                     family, gf)) {
	    ret = FcFalse;
	    goto bail;
	}
    }

bail:
    if (lset)
	FcStrSetDestroy (lset);

    return ret;
}

FcChar8 *
FcConfigFileGenerate (FcConfig      *config,
                      FcPattern     *pat,
                      const FcChar8 *font_path)
{
    FcFontSet *fs = NULL;
    FcChar8   *ret;
    FcBool     failed = FcFalse;
    int        i;
    FcStrBuf   buf;

    FcStrBufInit (&buf, NULL, 0);
    fs = FcFontSetCreate();

    if (!FcFileIsDir (font_path)) {
	FcFileScan (fs, NULL, NULL, NULL, font_path, FcTrue);
    } else {
	FcStrSet  *dirs = FcStrSetCreate();
	FcStrList *dirlist = FcStrListCreate (dirs);

	do {
	    FcDirScan (fs, dirs, NULL, NULL, font_path, FcTrue);
	} while ((font_path = FcStrListNext (dirlist)));

	FcStrListDone (dirlist);
	FcStrSetDestroy (dirs);
    }
    if (fs->nfont > 0) {
	FcHashTable *record = NULL;
	FcChar8     *target_family = NULL;
	FcBool       found = FcFalse;

	FcPatternObjectGetString (pat, FC_FAMILY_OBJECT, 0, &target_family);

	FcStrBufString (&buf,
	                (const FcChar8 *)
	                    "<?xml version=\"1.0\"?>\n"
	                    "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n"
	                    "<fontconfig>\n");

	record = FcHashTableCreate ((FcHashFunc)FcStrHashIgnoreBlanksAndCase,
	                            (FcCompareFunc)FcStrCmpIgnoreBlanksAndCase,
	                            NULL,
	                            NULL,
	                            NULL,
	                            (FcDestroyFunc)FcPatternDestroy);
	for (i = 0; i < fs->nfont; i++) {
	    FcChar8   *family;
	    FcPattern *p = NULL;

	    p = fs->fonts[i];
	    if (FcPatternObjectGetString (fs->fonts[i],
	                                  FC_FAMILY_OBJECT,
	                                  0,
	                                  &family) != FcResultMatch) {
		continue;
	    }
	    if (target_family &&
	        FcStrCmpIgnoreBlanksAndCase (family, target_family) != 0) {
		continue;
	    }
	    found = FcTrue;
	    if (!FcHashTableFind (record, family, (void **)&p)) {
		FcPatternReference (p);
		FcHashTableReplace (record, (void *)family, (void *)p);
		if (!FcConfigFileGenerateGenericAlias (config, pat, p, &buf)) {
		    failed = FcTrue;
		    goto bail;
		}
	    }
	}
	FcHashTableDestroy (record);

	if (target_family && !found) {
	    fprintf (stderr, "Fontconfig warning: %s: family not found in %s\n",
	             target_family, font_path);
	    failed = FcTrue;
	    goto bail;
	}

	FcStrBufString (&buf, (const FcChar8 *)"</fontconfig>\n");
    }
bail:
    FcFontSetDestroy (fs);

    ret = FcStrBufDone (&buf);
    if (failed && ret) {
	free (ret);
	ret = NULL;
    }

    return ret;
}

#define __fcconffile__
#include "fcaliastail.h"
#include "fcftaliastail.h"
#undef __fcconffile__
