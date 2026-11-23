#!/usr/bin/env python3
"""Cross-check every printf-family call site against the arguments it receives.

Why this exists
---------------
Version 2.0.0 shipped a crash that neither the compiler, /analyze nor the unit
tests could see. The evidence string used to explain a blocked click was

    [STR_EVIDENCE_ENGINE_FMT] = L" (engine judged by %s)"

while the call site passed a millisecond count:

    StringCchPrintfW(engine, ARRAYSIZE(engine), lang_str(STR_EVIDENCE_ENGINE_FMT),
                     (int)evidence->elapsed_ms);

printf read the number as a string pointer and walked into that address. The
test that was supposed to protect the translation tables only compared English
with Chinese, so two identically wrong strings passed.

This tool compares a format string against the call site, which is the check
that was missing. It resolves lang_str(STR_X) through both tables, and takes the
type of each argument from its declaration rather than from its name.

Usage
-----
    python tests/tools/audit_printf.py

Exit code 0 means every call site agrees with its format string.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# name -> (index of the format argument, index of the first variadic argument)
# Wrappers belong here too: they forward a printf format, and a mismatch inside
# one crashes exactly like a direct call.
PRINTF_FUNCS = {
    "StringCchPrintfW": (2, 3),
    "StringCchPrintfA": (2, 3),
    "swprintf": (2, 3),
    "swprintf_s": (3, 4),
    "_snwprintf": (2, 3),
    "_snwprintf_s": (4, 5),
    "append": (2, 3),
    "ShowErrorMessageBox": (0, 1),
}

INT_TYPES = (
    "int|unsigned|long|short|DWORD|UINT|ULONG|LONG|WORD|BYTE|BOOL|size_t|SSIZE_T|"
    "HRESULT|bool|uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t|"
    "LONGLONG|ULONGLONG|ATOM|COLORREF|WPARAM|LPARAM|LRESULT|UINT_PTR|INT_PTR"
)
STR_TYPES = (
    "wchar_t|char|WCHAR|CHAR|TCHAR|LPCWSTR|LPWSTR|LPCSTR|LPSTR|LPCTSTR|LPTSTR|"
    "PCWSTR|PWSTR|PWCHAR|PCHAR"
)

# Name based hints, only used when no declaration was found
STRING_HINTS = (
    "lang_str(", "lang_button_name(", "lang_format_", "block_log_button_slug",
    "debounce_reason_slug", "APP_VERSION_STRING", "APP_ORIGIN_URL", "APP_NAME",
    '"', "path", "directory", "module", "text", "buffer", "line", "label",
    "name", "message", "verdict", "slug", "caption", "title", "summary",
    "combined", "state_text", "value_text",
)


def strip_comments(text):
    """Replace comments with spaces, keeping offsets and newlines intact."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append(" ")
            out.append(" ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append(" ")
                out.append(" ")
                i += 2
        elif c == '"':
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i])
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                out.append(text[i])
                if text[i] == '"':
                    i += 1
                    break
                i += 1
        elif c == "'":
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i])
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                out.append(text[i])
                if text[i] == "'":
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_args(text, start):
    """Split the argument list that begins just after an opening paren."""
    args = []
    depth = 0
    current = []
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            current.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    current.append(text[i])
                    i += 1
                    if i < n:
                        current.append(text[i])
                        i += 1
                    continue
                current.append(text[i])
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c in "([{":
            depth += 1
            current.append(c)
        elif c in ")]}":
            if depth == 0:
                break
            depth -= 1
            current.append(c)
        elif c == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(c)
        i += 1
    tail = "".join(current).strip()
    if tail:
        args.append(tail)
    return args


def placeholders(fmt):
    """Return the conversion letters of a printf format, in order."""
    result = []
    i = 0
    n = len(fmt)
    while i < n:
        if fmt[i] != "%":
            i += 1
            continue
        i += 1
        if i < n and fmt[i] == "%":
            i += 1
            continue
        while i < n and fmt[i] in "-+ #0'":
            i += 1
        while i < n and (fmt[i].isdigit() or fmt[i] == "*"):
            i += 1
        if i < n and fmt[i] == ".":
            i += 1
            while i < n and (fmt[i].isdigit() or fmt[i] == "*"):
                i += 1
        if i < n and fmt[i] in "hlLwjztI":
            m = re.match(r"I(?:32|64)|hh|h|ll|l|L|w|j|z|t", fmt[i:])
            if m:
                i += m.end()
        if i >= n:
            break
        conv = fmt[i]
        if conv in "sS":
            result.append("s")
        elif conv in "diuoxX":
            result.append("n")
        elif conv == "c":
            result.append("c")
        elif conv == "p":
            result.append("p")
        elif conv in "fFeEgGaA":
            result.append("f")
        else:
            result.append("?" + conv)
        i += 1
    return result


def string_literal_value(expr):
    """Join adjacent wide/plain literals such as L"a" L"b" into one string."""
    parts = re.findall(r'L?"((?:[^"\\]|\\.)*)"', expr)
    if not parts:
        return None
    return "".join(parts)


def load_language_tables(path):
    """Map STR_NAME -> {'EN': ..., 'ZH': ...} for the two string tables."""
    text = open(path, encoding="utf-8").read()
    tables = {}
    for lang, marker in (("EN", "STRINGS_EN"), ("ZH", "STRINGS_ZH_CN")):
        start = text.find(marker)
        if start < 0:
            continue

        # Bound the scan to this table. Without the bound the English pass keeps
        # reading into the Chinese table and overwrites its own values, which
        # makes the two languages look identical and hides the drift we look for.
        brace = text.find("{", start)
        end = text.find("};", brace)
        body = text[brace : end if end > 0 else len(text)]

        for m in re.finditer(r"\[(STR_[A-Z0-9_]+)\]\s*=\s*((?:L\"(?:[^\"\\]|\\.)*\"\s*)+)", body):
            tables.setdefault(m.group(1), {})[lang] = string_literal_value(m.group(2))

    return tables


def load_declarations(text):
    """Map identifier -> 's' or 'n' from its declaration in the same file.

    The name alone is not enough: a variable called engine_ms is obviously a
    number to a human and opaque to a regex. The type is what the format string
    has to agree with.
    """
    decls = {}
    pattern = (
        r"\b(" + STR_TYPES + r"|" + INT_TYPES + r")\b"
        r"\s*(?:const\s*)?(\*+\s*)?(?:const\s*)?(\w+)\s*(?:\[|\s*=[^=]|;|,|\))"
    )
    for m in re.finditer(pattern, text):
        typ, name = m.group(1), m.group(3)
        if name in ("if", "for", "while", "return", "sizeof", "switch", "case"):
            continue
        if re.fullmatch(STR_TYPES, typ):
            decls.setdefault(name, "s")
        elif re.fullmatch(INT_TYPES, typ):
            decls.setdefault(name, "n")
    return decls


def classify(expr, decls):
    stripped = expr.strip()

    # A bare identifier: trust its declaration over any name based guess
    if re.fullmatch(r"[A-Za-z_]\w*", stripped) and stripped in decls:
        return decls[stripped]

    if stripped.startswith("&"):
        return "n"

    if re.match(r"^[0-9]+[uUlL]*$", stripped):
        return "n"

    if re.match(
        r"^\(\s*(int|unsigned(\s+(long|int|char|short))?|long|DWORD|UINT|ULONG|LONG|"
        r"WORD|size_t|bool|u?int(8|16|32|64)_t)\s*\)",
        stripped,
    ):
        return "n"

    if re.match(r'^L?"', stripped):
        return "s"

    # A conditional whose branches are string literals is a string, and the
    # sizeof rule below would otherwise grab it first
    if '"' in stripped:
        return "s"

    if re.fullmatch(r"\s*(sizeof|ARRAYSIZE|wcslen|strlen)\s*\(.*\)\s*", stripped):
        return "n"

    low = stripped.lower()
    for hint in STRING_HINTS:
        if hint in low:
            return "s"

    return "?"


def collect_sources():
    files = []
    for base, dirs, names in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ("bin", "obj", ".vs", ".codebuddy", "Log")]
        for name in names:
            if name.endswith(".c"):
                files.append(os.path.join(base, name))
    return sorted(files)


def main():
    language_path = os.path.join(ROOT, "src", "ui", "language.c")
    if not os.path.exists(language_path):
        print("run this from a MouseMend checkout: language.c not found")
        return 2

    lang = load_language_tables(language_path)

    problems = []
    unresolved = []
    checked = 0
    files = collect_sources()

    for path in files:
        raw = open(path, encoding="utf-8", errors="replace").read()
        text = strip_comments(raw)
        decls = load_declarations(text)
        rel = os.path.relpath(path, ROOT).replace("\\", "/")

        for func, (fmt_index, variadic_index) in PRINTF_FUNCS.items():
            for m in re.finditer(r"\b" + func + r"\s*\(", text):
                checked += 1
                open_paren = text.index("(", m.end() - 1)
                args = split_args(text, open_paren + 1)
                line_no = text.count("\n", 0, m.start()) + 1

                if len(args) <= fmt_index:
                    problems.append((rel, line_no, func, "too few arguments"))
                    continue

                fmt_expr = args[fmt_index]
                literal = string_literal_value(fmt_expr)

                if literal is None:
                    lm = re.match(r"lang_str\(\s*(STR_[A-Z0-9_]+)\s*\)$", fmt_expr.strip())
                    if lm:
                        ident = lm.group(1)
                        entry = lang.get(ident, {})
                        literal = entry.get("EN") or entry.get("ZH")
                        en = entry.get("EN")
                        zh = entry.get("ZH")
                        if en is not None and zh is not None:
                            if placeholders(en) != placeholders(zh):
                                problems.append(
                                    (rel, line_no, func,
                                     f"{ident}: EN {placeholders(en)} vs ZH {placeholders(zh)}")
                                )
                        if literal is None:
                            unresolved.append((rel, line_no, func, f"{ident} missing"))
                            continue
                    else:
                        # Declared in a header, or a parameter: nothing to compare
                        unresolved.append((rel, line_no, func, fmt_expr))
                        continue

                ph = placeholders(literal)
                variadic = args[variadic_index:]

                if len(ph) != len(variadic):
                    problems.append(
                        (rel, line_no, func,
                         f"{len(ph)} placeholders {ph} but {len(variadic)} arguments")
                    )
                    continue

                for want, got_expr in zip(ph, variadic):
                    got = classify(got_expr, decls)
                    if want == "n" and got == "s":
                        problems.append(
                            (rel, line_no, func,
                             f"placeholder is numeric but the argument looks like text: {got_expr}")
                        )
                    elif want == "s" and got == "n":
                        problems.append(
                            (rel, line_no, func,
                             f"placeholder is %s but the argument is numeric: {got_expr}")
                        )

    print(f"files scanned     : {len(files)}")
    print(f"call sites checked: {checked}")
    print()

    if problems:
        print("=== PROBLEMS ===")
        for rel, line, func, why in problems:
            print(f"  {rel}:{line}  {func}")
            print(f"      {why}")
    else:
        print("=== PROBLEMS: none ===")

    if unresolved:
        print()
        print("=== not checked automatically (format is a parameter) ===")
        for rel, line, func, why in unresolved:
            print(f"  {rel}:{line}  {func}  {why}")

    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
