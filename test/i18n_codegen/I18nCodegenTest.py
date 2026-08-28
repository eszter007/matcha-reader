#!/usr/bin/env python3

import importlib.util
import os
import sys
import tempfile
from pathlib import Path


script_path = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("gen_i18n", script_path)
gen_i18n = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_i18n)

languages = ["EN"]
language_names = ["English"]
keys = ["STR_FIRST"]
translations = {"STR_FIRST": ["First"]}
builtin = {"EN"}
fingerprint = gen_i18n._table_fingerprint(
    languages, language_names, keys, translations, builtin
)

with tempfile.TemporaryDirectory() as temp_dir:
    output = Path(temp_dir)
    keys_path = output / "I18nKeys.h"
    strings_path = output / "I18nStrings.cpp"
    gen_i18n.generate_keys_header(
        languages, language_names, ["en"], keys, fingerprint, str(keys_path), builtin=builtin
    )
    gen_i18n.generate_strings_cpp(
        languages,
        language_names,
        keys,
        translations,
        fingerprint,
        str(strings_path),
        builtin=builtin,
    )

    guard = f"requireTable_{fingerprint}"
    assert guard in keys_path.read_text(encoding="utf-8")
    assert guard in strings_path.read_text(encoding="utf-8")

    os.utime(keys_path, ns=(1_000_000_000, 1_000_000_000))
    original_mtime = keys_path.stat().st_mtime_ns
    gen_i18n.generate_keys_header(
        languages, language_names, ["en"], keys, fingerprint, str(keys_path), builtin=builtin
    )
    assert keys_path.stat().st_mtime_ns == original_mtime

changed = gen_i18n._table_fingerprint(
    languages, language_names, keys, {"STR_FIRST": ["Changed"]}, builtin
)
assert changed != fingerprint
