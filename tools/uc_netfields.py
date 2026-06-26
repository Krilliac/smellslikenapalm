#!/usr/bin/env python3
r"""UE3 ClassNetCache net-field counter for the RS2 decompiled UnrealScript.

Computes, per the algorithm in docs/UE3_ClassNetCache_HandleOrder.md
(UnClass.cpp:2126-2152 + UnCoreNet.cpp GetClassNetCache):

  NetFields of a class = (CPF_Net properties: vars named in its replication{} block)
                       + (FUNC_Net functions: client/server function|event FIRST declared
                          in this class, i.e. name not declared in ANY ancestor).
  maxHandle(C) = sum over C and all its ancestors of len(NetFields(class)).
  handle(field) = FieldsBase(class) + field's index within class NetFields (declaration order).

maxHandle is a pure COUNT and is therefore independent of intra-class ordering, which the
decompiled source cannot pin exactly (NetIndex is assigned by the script compiler). So we
report maxHandle as the trustworthy number, plus a best-effort declaration-order list for
ROPlayerController (source line order) to locate ChangedTeams' handle as a *hint* to confirm
empirically against the capture.
"""
import re, sys

SRC = r"D:\RE-Tools\rs2-source"

# chain super-first
CHAIN = [
    ("Object",               r"Core\Object.uc"),
    ("Actor",                r"Engine\Actor.uc"),
    ("Controller",           r"Engine\Controller.uc"),
    ("PlayerController",      r"Engine\PlayerController.uc"),
    ("GamePlayerController",  r"GameFramework\GamePlayerController.uc"),
    ("ROPlayerController",    r"ROGame\ROPlayerController.uc"),
]

IDENT = r"[A-Za-z_]\w*"
FUNC_DECL = re.compile(
    r"^(?P<pre>[^/\\\n]*?)\b(?P<kw>function|event)\b\s+(?P<rest>.*?)\(",
)

def strip_comments(text):
    # decompiler line comments start with backslash:  \ Pos:0x000   \ Export ...
    lines = []
    for ln in text.splitlines():
        s = ln
        # kill // comments
        ci = s.find("//")
        if ci >= 0:
            s = s[:ci]
        # kill decompiler backslash comment lines/segments
        bi = s.find("\\")
        if bi >= 0:
            s = s[:bi]
        lines.append(s)
    text = "\n".join(lines)
    # block comments
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return text

def find_block(text, header):
    """Return body between the brace pair following a top-level `header` keyword."""
    m = re.search(r"(?m)^\s*" + header + r"\s*\{", text)
    if not m:
        return None
    i = m.end() - 1  # at '{'
    depth = 0
    for j in range(i, len(text)):
        c = text[j]
        if c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[i+1:j]
    return None

def replicated_vars(text):
    """Names at paren-depth 0 in the replication{} block (excluding 'if')."""
    body = find_block(text, "replication")
    if body is None:
        return []
    names, depth, i, n = [], 0, 0, len(body)
    token = re.compile(IDENT)
    while i < n:
        c = body[i]
        if c == "(":
            depth += 1; i += 1; continue
        if c == ")":
            depth -= 1; i += 1; continue
        if depth == 0:
            m = token.match(body, i)
            if m:
                w = m.group(0)
                if w != "if":
                    names.append(w)
                i = m.end(); continue
        i += 1
    # de-dup preserving order (a name could appear under two conditions; still 1 field)
    seen, out = set(), []
    for w in names:
        if w not in seen:
            seen.add(w); out.append(w)
    return out

def func_decls(text):
    """All function/event declarations: list of (name, is_net, lineno)."""
    out = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        # need '(' on same line as the keyword+name
        m = FUNC_DECL.search(raw)
        if not m:
            continue
        pre = m.group("pre")
        rest = m.group("rest")  # between keyword and '(' : optional return type + name
        # name is last identifier before '('
        ids = re.findall(IDENT, rest)
        if not ids:
            continue
        name = ids[-1]
        pre_words = set(re.findall(IDENT, pre))
        is_net = ("client" in pre_words) or ("server" in pre_words)
        out.append((name, is_net, lineno))
    return out

def main():
    ancestors_funcnames = set()   # every function/event name declared in classes processed so far
    fields_base = 0
    per_class = []
    ro_ordered = None
    for cname, rel in CHAIN:
        path = SRC + "\\" + rel
        text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        props = replicated_vars(text)
        decls = func_decls(text)
        all_names_here = set(n for (n, _, _) in decls)
        net_funcs = [(n, ln) for (n, isnet, ln) in decls
                     if isnet and n not in ancestors_funcnames]
        # de-dup net funcs by name within class (an override-with-spec can't happen for
        # first-declared; but guard against repeated forward decls)
        seen, nf = set(), []
        for n, ln in net_funcs:
            if n not in seen:
                seen.add(n); nf.append((n, ln))
        count = len(props) + len(nf)
        per_class.append((cname, len(props), len(nf), count, fields_base, props, nf))
        if cname == "ROPlayerController":
            # build declaration-order net field list (props by var line, funcs by line)
            var_line = {}
            for ln, raw in enumerate(text.splitlines(), 1):
                mm = re.match(r"\s*var\b", raw)
                if mm:
                    for nm in re.findall(IDENT, raw):
                        var_line.setdefault(nm, ln)
            ordered = []
            for p in props:
                ordered.append((var_line.get(p, 10**9), "prop", p))
            for n, ln in nf:
                ordered.append((ln, "func", n))
            ordered.sort()
            ro_ordered = (fields_base, ordered)
        ancestors_funcnames |= all_names_here
        fields_base += count

    max_handle = fields_base
    # build GLOBAL ordered handle->field map across all classes (for empirical id)
    if len(sys.argv) > 1 and sys.argv[1] == "-g":
        gh = 0
        for (cname, np_, nf_, count, fb, props, nf) in per_class:
            path = SRC + "\\" + dict(CHAIN)[cname]
            text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
            var_line = {}
            for ln, raw in enumerate(text.splitlines(), 1):
                if re.match(r"\s*var\b", raw):
                    for nm in re.findall(IDENT, raw):
                        var_line.setdefault(nm, ln)
            ordered = sorted([(var_line.get(p, 10**9), "prop", p) for p in props] +
                             [(ln, "func", n) for n, ln in nf])
            for (ln, kind, nm) in ordered:
                print(f"{gh:4} {cname:20} {kind:4} {nm}")
                gh += 1
        return
    print(f"{'class':22} {'netProps':>9} {'netFuncs':>9} {'count':>6} {'FieldsBase':>11}")
    for (cname, np_, nf_, count, fb, props, nf) in per_class:
        print(f"{cname:22} {np_:9} {nf_:9} {count:6} {fb:11}")
    print(f"\nmaxHandle (ROPlayerController GetMaxIndex) = {max_handle}")

    if ro_ordered:
        fb, ordered = ro_ordered
        print(f"\nROPlayerController FieldsBase = {fb}; its NetFields (declaration order):")
        for idx, (ln, kind, nm) in enumerate(ordered):
            handle = fb + idx
            mark = "   <-- ChangedTeams" if nm == "ChangedTeams" else ""
            print(f"  handle {handle:4}  ({kind:4} L{ln:<5}) {nm}{mark}")

    # show the ROPlayerController net function set for sanity
    if len(sys.argv) > 1 and sys.argv[1] == "-v":
        for (cname, np_, nf_, count, fb, props, nf) in per_class:
            print(f"\n== {cname}: net props ({len(props)}) ==")
            print("  " + ", ".join(props))
            print(f"== {cname}: net funcs first-declared ({len(nf)}) ==")
            print("  " + ", ".join(n for n, _ in nf))

if __name__ == "__main__":
    main()
