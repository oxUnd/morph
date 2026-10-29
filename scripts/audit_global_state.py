#!/usr/bin/env python3
"""Reject mutable static-storage variables in project-owned C/C++ code.

Usage: python3 scripts/audit_global_state.py --build-dir build
Requires compile_commands.json and libclang (LIBCLANG_LIBRARY may name it).
Read-only data and vendor/dependency headers are excluded. Function-local static
variables, TLS, macros and project headers are inspected using Clang's AST.
"""
import argparse
import ctypes as C
import ctypes.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


class Cursor(C.Structure):
    _fields_ = [("kind", C.c_uint), ("xdata", C.c_int), ("data", C.c_void_p * 3)]


class Type(C.Structure):
    _fields_ = [("kind", C.c_int), ("data", C.c_void_p * 2)]


class String(C.Structure):
    _fields_ = [("data", C.c_void_p), ("flags", C.c_uint)]


class Location(C.Structure):
    _fields_ = [("ptr", C.c_void_p * 2), ("value", C.c_uint)]


def library_path():
    override = os.environ.get("LIBCLANG_LIBRARY")
    if override:
        return override
    found = ctypes.util.find_library("clang")
    if found:
        return found
    if sys.platform == "darwin":
        developer = subprocess.check_output(["xcode-select", "-p"], text=True).strip()
        for suffix in ("usr/lib/libclang.dylib", "Toolchains/XcodeDefault.xctoolchain/usr/lib/libclang.dylib"):
            candidate = Path(developer) / suffix
            if candidate.exists():
                return str(candidate)
    raise RuntimeError("libclang not found; set LIBCLANG_LIBRARY to its shared library")


class Clang:
    def __init__(self):
        self.lib = C.CDLL(library_path())
        self.visit_type = C.CFUNCTYPE(C.c_uint, Cursor, Cursor, C.c_void_p)
        self.bind("createIndex", C.c_void_p, C.c_int, C.c_int)
        self.bind("disposeIndex", None, C.c_void_p)
        self.bind("parseTranslationUnit", C.c_void_p, C.c_void_p, C.c_char_p,
                  C.POINTER(C.c_char_p), C.c_int, C.c_void_p, C.c_uint, C.c_uint)
        self.bind("disposeTranslationUnit", None, C.c_void_p)
        self.bind("getTranslationUnitCursor", Cursor, C.c_void_p)
        self.bind("visitChildren", C.c_uint, Cursor, self.visit_type, C.c_void_p)
        self.bind("getCursorLocation", Location, Cursor)
        self.bind("getExpansionLocation", None, Location, C.POINTER(C.c_void_p),
                  C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint))
        self.bind("getSpellingLocation", None, Location, C.POINTER(C.c_void_p),
                  C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint))
        self.bind("getFileName", String, C.c_void_p)
        self.bind("getCursorSpelling", String, Cursor)
        self.bind("getCString", C.c_char_p, String)
        self.bind("disposeString", None, String)
        self.bind("getCursorType", Type, Cursor)
        self.bind("getCanonicalType", Type, Type)
        self.bind("getTypeDeclaration", Cursor, Type)
        self.bind("CXXField_isMutable", C.c_uint, Cursor)
        self.bind("getTypeSpelling", String, Type)
        self.bind("isConstQualifiedType", C.c_uint, Type)
        self.bind("getArrayElementType", Type, Type)
        self.bind("Cursor_getStorageClass", C.c_uint, Cursor)
        self.bind("isCursorDefinition", C.c_uint, Cursor)
        self.bind("Cursor_hasVarDeclGlobalStorage", C.c_int, Cursor)
        self.bind("getNumDiagnostics", C.c_uint, C.c_void_p)
        self.bind("getDiagnostic", C.c_void_p, C.c_void_p, C.c_uint)
        self.bind("getDiagnosticSeverity", C.c_uint, C.c_void_p)
        self.bind("formatDiagnostic", String, C.c_void_p, C.c_uint)
        self.bind("disposeDiagnostic", None, C.c_void_p)

    def bind(self, name, result, *args):
        function = getattr(self.lib, "clang_" + name)
        function.restype, function.argtypes = result, args
        setattr(self, name, function)

    def string(self, value):
        result = (self.getCString(value) or b"").decode(errors="replace")
        self.disposeString(value)
        return result

    def has_mutable_field(self, kind):
        kind = self.getCanonicalType(kind)
        while kind.kind in (112, 114, 115, 116):
            kind = self.getArrayElementType(kind)
        if kind.kind != 105:  # Record; pointer targets have separate storage.
            return False
        mutable = False

        @self.visit_type
        def inspect(cursor, _parent, _data):
            nonlocal mutable
            if cursor.kind == 6:  # FieldDecl
                mutable = bool(self.CXXField_isMutable(cursor)) or self.has_mutable_field(
                    self.getCursorType(cursor))
            elif cursor.kind == 44:  # CXXBaseSpecifier
                mutable = self.has_mutable_field(self.getCursorType(cursor))
            return 0 if mutable else 1

        self.visitChildren(self.getTypeDeclaration(kind), inspect, None)
        return mutable

    def immutable(self, kind):
        kind = self.getCanonicalType(kind)
        qualified = False
        while True:
            qualified |= bool(self.isConstQualifiedType(kind))
            if kind.kind not in (112, 114, 115, 116):
                break
            kind = self.getArrayElementType(kind)
        return qualified and not self.has_mutable_field(kind)

    def audit(self, entry, root):
        args = entry.get("arguments") or shlex.split(entry["command"])
        filtered, skip = [], False
        source = Path(entry["file"]).resolve()
        for arg in args[1:]:
            if skip:
                skip = False
                continue
            if arg in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
            elif arg not in ("-c", "-MD", "-MMD") and Path(arg).resolve() != source:
                filtered.append(arg)
        resource = subprocess.check_output([args[0], "-print-resource-dir"], text=True).strip()
        filtered.extend(["-resource-dir", resource])
        if sys.platform == "darwin" and "-isysroot" not in filtered:
            sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
            filtered.extend(["-isysroot", sdk])
        array = (C.c_char_p * len(filtered))(*(arg.encode() for arg in filtered))
        index = self.createIndex(0, 0)
        unit = self.parseTranslationUnit(index, str(source).encode(), array,
                                         len(filtered), None, 0, 0)
        if not unit:
            self.disposeIndex(index)
            raise RuntimeError(f"cannot parse {source}")
        hits, failures = [], []
        try:
            for number in range(self.getNumDiagnostics(unit)):
                diagnostic = self.getDiagnostic(unit, number)
                if self.getDiagnosticSeverity(diagnostic) >= 3:
                    failures.append(self.string(self.formatDiagnostic(diagnostic, 3)))
                self.disposeDiagnostic(diagnostic)
            if failures:
                raise RuntimeError("\n".join(failures))

            @self.visit_type
            def visit(cursor, _parent, _data):
                location = self.getCursorLocation(cursor)
                file, line = C.c_void_p(), C.c_uint()
                self.getExpansionLocation(location, C.byref(file), C.byref(line), None, None)
                path = Path(self.string(self.getFileName(file))) if file.value else None
                owned = path and path.is_relative_to(root) and path.relative_to(root).parts[0] in (
                    "src", "tests", "exts", "examples")
                if not owned:
                    return 1
                if cursor.kind == 9:
                    spelling_file = C.c_void_p()
                    self.getSpellingLocation(location, C.byref(spelling_file), None, None, None)
                    spelling = (Path(self.string(self.getFileName(spelling_file)))
                                if spelling_file.value else None)
                    if spelling and (not spelling.is_relative_to(root) or
                                     spelling.relative_to(root).parts[0] not in
                                     ("src", "tests", "exts", "examples")):
                        return 1
                if (cursor.kind == 9 and (self.Cursor_getStorageClass(cursor) != 2
                        or self.isCursorDefinition(cursor))
                        and self.Cursor_hasVarDeclGlobalStorage(cursor)
                        and not self.immutable(self.getCursorType(cursor))):
                    hits.append((str(path.relative_to(root)), line.value,
                                 self.string(self.getCursorSpelling(cursor)),
                                 self.string(self.getTypeSpelling(self.getCursorType(cursor)))))
                return 2

            self.visitChildren(self.getTranslationUnitCursor(unit), visit, None)
            return hits
        finally:
            self.disposeTranslationUnit(unit)
            self.disposeIndex(index)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    entries = json.loads((options.build_dir / "compile_commands.json").read_text())
    compiled = {Path(entry["file"]).resolve() for entry in entries}
    template = next(entry for entry in entries
                    if entry["file"].endswith("/src/agent/tools/file_read.c"))
    template_args = template.get("arguments") or shlex.split(template["command"])
    extras = []
    for directory in ("src", "tests", "exts", "examples"):
        for source in sorted((root / directory).rglob("*")):
            if source.suffix != ".c" or source.resolve() in compiled:
                continue
            args = [arg for arg in template_args if arg != template["file"]]
            if sys.platform == "darwin" and source == root / "exts/locate/locate.c":
                args.extend(["-x", "objective-c"])
            extras.append(dict(file=str(source), directory=template["directory"],
                               arguments=args + [str(source)]))
    entries = extras + entries
    clang = Clang()
    hits, seen = set(), set()
    for entry in entries:
        source = Path(entry["file"]).resolve()
        if (source in seen or not source.is_relative_to(root)
                or source.relative_to(root).parts[0] not in ("src", "tests", "exts", "examples")):
            continue
        seen.add(source)
        print(f"Auditing {source.relative_to(root)}", file=sys.stderr, flush=True)
        previous = Path.cwd()
        try:
            os.chdir(entry["directory"])
            hits.update(clang.audit(entry, root))
        finally:
            os.chdir(previous)
    for file, line, name, kind in sorted(hits):
        print(f"{file}:{line}: mutable static storage: {name} ({kind})")
    print(f"{len(hits)} mutable declarations in {len(seen)} translation units")
    return bool(hits)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
