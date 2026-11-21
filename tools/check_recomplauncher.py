#!/usr/bin/env python3
"""Checks recomplauncher.json, the file RecompLauncher sets the game up from, against RecompLauncher's own JSON schema and against this game.

    check_recomplauncher.py <recomplauncher.json> (--guide <recomplauncher-agent.md> | --schema <schema.json> | --no-schema)
                            [--version <x.y.z>] [--repo <this repo>] [--self-test] [--describe <settings.json>]

The schema is not kept in this repository: it is part of RecompLauncher's developer guide (GPL-3), also published at
https://recomplauncher.com/schema/recomplauncher.json. Give it as the guide (its "# The JSON schema" section, the first fenced json
block after it; RECOMPLAUNCHER_GUIDE in the environment says where the guide is, when --guide is not given) or as the schema file. This
script holds a small validator of its own (the parts of JSON Schema draft 7 that schema uses), not a copy of the schema. Without either,
--no-schema checks only the second half below, and says the first was skipped.

What it checks:
  1. the file is JSON and the schema accepts it (an unknown key anywhere the schema closes an object, such as at the top, fails)
  2. every path and placeholder in it is one RecompLauncher fills in, and no @...@ left from tools/recomplauncher.json.in
  3. the start and setup steps run the same Buffy.exe and pass what Buffy.exe takes (main.cpp), the setup's version is the release's
     (src/common/game_info.cpp, or --version), and its "done" file is the one the build makes (<exe name>.dll in the data folder's build)
  4. every exit code app.h names has a message (or "ok" for 0) in exit_codes
  5. each setting names a key the game's settings list has (sdk/runtime/app/settings_list.cpp), writes values the game reads back (the type's
     form: true, an integer, a quoted choice in the list), stays in the key's range, and its defaults are the game's own (sdk/runtime/app/app.h,
     settings.cpp). The Windows default display mode is known here, as the Mac's differs.
  --self-test also runs a few files the schema must refuse (an unknown top-level key, ...) and fails if one is accepted.

The Mac's file (tools/recomplauncher-mac.json.in: its "system" has macos) is checked the same way, with the Mac's differences: the program is
Buffy.app/Contents/MacOS/Buffy and takes what sdk/host/main.cpp takes, the game's code is <name>.dylib, and the settings are checked against
--describe <settings.json>, what the Mac program's own --describe-settings prints (tools/package_mac.sh puts it in the app as
Contents/Resources/settings.json): every key a setting writes must be one the program lists, with its type, range, choices and default.
A key it does not list (the Windows renderer, which the Mac program has no choice of, among them) is refused.
"""
import argparse
import json
import os
import re
import sys

PLACEHOLDER = re.compile(r"\{([a-z_]+)(?::[A-Za-z0-9_-]+)?\}")
PLACEHOLDERS = {"install", "data", "game", "documents", "local", "config", "home", "pad", "input", "value"}


# ---- a validator for the parts of JSON Schema draft 7 that RecompLauncher's schema uses -------------------------------------------------

def type_ok(value, name):
    if name == "object":
        return isinstance(value, dict)
    if name == "array":
        return isinstance(value, list)
    if name == "string":
        return isinstance(value, str)
    if name == "boolean":
        return isinstance(value, bool)
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    raise SystemExit("this validator does not know the type " + name)


class Validator:
    # keywords it acts on; one it does not know (and the schema is not just describing with) stops the check, so that nothing passes unchecked
    KNOWN = {"$schema", "$id", "$ref", "title", "description", "default", "examples", "type", "enum", "const", "pattern", "minLength",
             "minimum", "maximum", "exclusiveMinimum", "minItems", "maxItems", "uniqueItems", "items", "required", "properties",
             "additionalProperties", "propertyNames", "allOf", "anyOf", "if", "then", "not", "definitions"}

    def __init__(self, root):
        self.root = root

    def resolve(self, ref):
        if not ref.startswith("#/"):
            raise SystemExit("this validator reads only references inside the schema, not " + ref)
        node = self.root
        for part in ref[2:].split("/"):
            node = node[part]
        return node

    def check(self, value, schema, path, out):
        if "$ref" in schema:
            self.check(value, self.resolve(schema["$ref"]), path, out)
            return
        for key in schema:
            if key not in self.KNOWN:
                raise SystemExit("the schema uses '%s', which this validator does not act on" % key)
        if "type" in schema:
            names = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
            if not any(type_ok(value, n) for n in names):
                out.append("%s: not a %s" % (path, "/".join(names)))
                return
        if "const" in schema and value != schema["const"]:
            out.append("%s: must be %r" % (path, schema["const"]))
        if "enum" in schema and value not in schema["enum"]:
            out.append("%s: %r is not one of %s" % (path, value, ", ".join(map(repr, schema["enum"]))))
        if isinstance(value, str):
            if "pattern" in schema and not re.search(schema["pattern"], value):
                out.append("%s: %r does not match %s" % (path, value, schema["pattern"]))
            if "minLength" in schema and len(value) < schema["minLength"]:
                out.append("%s: shorter than %d" % (path, schema["minLength"]))
        if type_ok(value, "number"):
            if "minimum" in schema and value < schema["minimum"]:
                out.append("%s: below %s" % (path, schema["minimum"]))
            if "maximum" in schema and value > schema["maximum"]:
                out.append("%s: above %s" % (path, schema["maximum"]))
            if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
                out.append("%s: not above %s" % (path, schema["exclusiveMinimum"]))
        if isinstance(value, list):
            if "minItems" in schema and len(value) < schema["minItems"]:
                out.append("%s: fewer than %d items" % (path, schema["minItems"]))
            if "maxItems" in schema and len(value) > schema["maxItems"]:
                out.append("%s: more than %d items" % (path, schema["maxItems"]))
            if schema.get("uniqueItems") and len({json.dumps(v, sort_keys=True) for v in value}) != len(value):
                out.append("%s: items are not unique" % path)
            if "items" in schema:
                for i, v in enumerate(value):
                    self.check(v, schema["items"], "%s[%d]" % (path, i), out)
        if isinstance(value, dict):
            for key in schema.get("required", []):
                if key not in value:
                    out.append("%s: '%s' is missing" % (path, key))
            props = schema.get("properties", {})
            extra = schema.get("additionalProperties", True)
            for key, v in value.items():
                where = "%s/%s" % (path, key)
                if "propertyNames" in schema:
                    self.check(key, schema["propertyNames"], where + " (its name)", out)
                if key in props:
                    self.check(v, props[key], where, out)
                elif extra is False:
                    out.append("%s: '%s' is not a key this object takes" % (path, key))
                elif isinstance(extra, dict):
                    self.check(v, extra, where, out)
        for sub in schema.get("allOf", []):
            self.check(value, sub, path, out)
        if "anyOf" in schema:
            if not any(self.passes(value, sub) for sub in schema["anyOf"]):
                out.append("%s: matches none of the allowed shapes" % path)
        if "not" in schema and self.passes(value, schema["not"]):
            out.append("%s: matches a shape that is not allowed" % path)
        if "if" in schema and self.passes(value, schema["if"]) and "then" in schema:
            self.check(value, schema["then"], path, out)

    def passes(self, value, schema):
        problems = []
        self.check(value, schema, "", problems)
        return not problems


def load_schema(args):
    guide = args.guide or os.environ.get("RECOMPLAUNCHER_GUIDE")
    if args.schema:
        with open(args.schema, encoding="utf-8") as f:
            return json.load(f), args.schema
    if not guide:
        return None, None
    with open(guide, encoding="utf-8") as f:
        text = f.read()
    at = text.find("# The JSON schema")
    start = text.find("```json", at) if at >= 0 else -1
    end = text.find("```", start + 7) if start >= 0 else -1
    if end < 0:
        raise SystemExit("no json block after '# The JSON schema' in " + guide)
    return json.loads(text[start + 7:end]), guide + " (# The JSON schema)"


# ---- this game ---------------------------------------------------------------------------------------------------------------------------

def read(repo, *parts):
    with open(os.path.join(repo, *parts), encoding="utf-8") as f:
        return f.read()


def settings_list(repo):
    """section/key -> {type, member, choices, min, max} from settings_list.cpp's descriptions."""
    text = read(repo, "sdk", "runtime", "app", "settings_list.cpp")
    found = {}
    heads = list(re.finditer(r'\{"(\w+)",\s*"(\w+)",\s*(?:cfg::)?Type::(\w+),', text))
    for i, m in enumerate(heads):
        body = text[m.end():heads[i + 1].start() if i + 1 < len(heads) else len(text)]
        body = body.split("\n    };")[0].split("\n    l.push_back")[0]
        member = re.search(r"&s\.(\w+)", body)
        choices = re.search(r'\{((?:\s*"[^"]*"\s*,?)+)\}', body)
        limits = re.search(r"nullptr,\s*(-?\d+),\s*(-?\d+)", body)
        found["%s/%s" % (m.group(1), m.group(2))] = {
            "type": m.group(3),
            "member": "display_mode" if (m.group(1), m.group(2)) == ("display", "mode") else member.group(1) if member else None,
            "choices": re.findall(r'"([^"]*)"', choices.group(1)) if choices and m.group(3) == "Choice" else None,
            "limits": (int(limits.group(1)), int(limits.group(2))) if limits else None,
        }
    return found


def setting_defaults(repo):
    """member -> default, from app.h's Settings and the overrides at the top of settings.cpp (Windows: not the #ifndef _WIN32 ones)."""
    header = read(repo, "sdk", "runtime", "app", "app.h")
    body = header[header.index("struct Settings {"):]
    body = body[:body.index("\n};")]
    values = {}
    for m in re.finditer(r"^\s*(?:[\w:<>]+)\s+(\w+)\s*=\s*([^;/]+);", body, re.M):
        values[m.group(1)] = m.group(2).strip()
    cpp = read(repo, "sdk", "runtime", "app", "settings.cpp")
    block = cpp[cpp.index("Settings& Get() {"):]
    block = block[:block.index("}();")]
    skip = False
    for line in block.splitlines():
        s = line.strip()
        if s.startswith("#ifndef _WIN32"):
            skip = True
        elif s.startswith("#endif"):
            skip = False
        elif not skip and (m := re.match(r"d\.(\w+) = ([^;]+);", s)):
            values[m.group(1)] = m.group(2).strip()
    enum = re.search(r"enum class DisplayMode : uint32_t \{([^}]*)\}", header).group(1)
    modes = [n.strip().lower() for n in enum.split(",") if n.strip()]
    return values, modes


def split_key(key):
    section, _, name = key.partition("/")
    return section, name


def describe_list(path):
    """section/key -> {type, member, choices, limits, default}, as settings_list gives them, from what a program's --describe-settings printed.
    A float's limits are in hundredths and a default is written as the game writes it, as the checks of the Windows list expect."""
    if not path:
        raise SystemExit("the Mac's file is checked against the program's settings: give --describe <the --describe-settings output>")
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    kinds = {"bool": "Bool", "int": "Int", "float": "Float", "choice": "Choice", "string": "String"}
    found = {}
    for s in data["settings"]:
        kind = kinds[s["type"]]
        lo, hi, default = s.get("min"), s.get("max"), s["default"]
        if kind == "Bool":
            default = "true" if default else "false"
        elif kind == "Float":
            default = "%.2f" % default
        elif kind == "Choice":
            default = '"%s"' % default
        elif kind == "Int":
            default = str(default)
        limits = None
        if lo is not None and hi is not None:
            limits = (int(round(lo * 100)), int(round(hi * 100))) if kind == "Float" else (lo, hi)
        found["%s/%s" % (s["section"], s["key"])] = {"type": kind, "member": None, "limits": limits, "default": default,
                                                     "choices": s.get("choices") if kind == "Choice" else None}
    return found


def game_checks(m, args, problems, notes):
    repo = os.path.abspath(args.repo)
    mac = "macos" in m.get("system", {})
    # 2. placeholders and paths
    def walk(node, path):
        if isinstance(node, dict):
            for k, v in node.items():
                walk(v, path + "/" + k)
        elif isinstance(node, list):
            for i, v in enumerate(node):
                walk(v, "%s[%d]" % (path, i))
        elif isinstance(node, str):
            if "@" in node:
                problems.append("%s: '%s' has an @ in it (a placeholder of the .in file left?)" % (path, node))
            for p in PLACEHOLDER.findall(node):
                if p not in PLACEHOLDERS:
                    problems.append("%s: {%s} is not a placeholder RecompLauncher fills in" % (path, p))
    walk(m, "")
    for key in ("saves",):
        for p in m.get(key, []):
            if not p.startswith("{data}/"):
                problems.append("saves: %s is not under {data}, the one folder the game writes to" % p)
    for folder, places in m.get("folders", {}).items():
        for place in places:
            if not place["path"].startswith("{data}/"):
                problems.append("folders/%s: %s is not under {data}" % (folder, place["path"]))
    # 3. the two steps
    start, prepare = m.get("start", {}), m.get("prepare", {})
    exe = (start.get("program") or [""])[0]
    if prepare and prepare.get("program") != start.get("program"):
        problems.append("start and prepare run different programs")
    if mac:
        # the program's own path, which every RecompLauncher finds (a plain Buffy.app needs Beta 55 or newer)
        found = re.fullmatch(r"(\w+)\.app/Contents/MacOS/(\w+)", exe)
        if not found or found.group(1) != found.group(2):
            problems.append("the program %r is not <name>.app/Contents/MacOS/<name>" % exe)
        main_cpp, main_name, quote = read(repo, "sdk", "host", "main.cpp"), "sdk/host/main.cpp", 'a == "%s"'
    else:
        main_cpp, main_name, quote = read(repo, "sdk", "runtime", "app", "main.cpp"), "sdk/runtime/app/main.cpp", 'L"%s"'
    program = os.path.basename(exe)
    for step, a in (("start", start.get("args", [])), ("prepare", prepare.get("args", []))):
        for arg in a:
            option = arg.split("=")[0]
            if option.startswith("--") and quote % option not in main_cpp:
                problems.append("%s: %s takes no %s (%s)" % (step, program, option, main_name))
        if "--game={game}" not in a:
            problems.append("%s: --game={game} is not one argument" % step)
        if "--data" not in a or a[a.index("--data") + 1:a.index("--data") + 2] != ["{data}"]:
            problems.append("%s: --data {data} is missing" % step)
    if "--no-dialogs" not in start.get("args", []):
        problems.append("start: --no-dialogs is missing (a launcher's start has nobody to ask)")
    if "--prepare" not in prepare.get("args", []):
        problems.append("prepare: --prepare is missing")
    want_done = "{data}/build/%s.%s" % (os.path.splitext(program)[0], "dylib" if mac else "dll")
    if prepare and want_done not in prepare.get("done", []):
        problems.append("prepare.done does not hold %s, the file the build makes" % want_done)
    version = args.version
    if not version:
        version = re.search(r'\.version = "buffy-port ([^"]+)"', read(repo, "src", "common", "game_info.cpp")).group(1)
    if prepare and prepare.get("version") != version:
        problems.append("prepare.version is %r, the release is %s" % (prepare.get("version"), version))
    # 4. the exit codes
    codes = {int(n) for n in re.findall(r"\bkExit\w+ = (\d+),", read(repo, "sdk", "runtime", "app", "app.h"))}
    for code in sorted(codes):
        if str(code) not in m.get("exit_codes", {}):
            problems.append("exit_codes has no message for %d, which app.h names" % code)
    for code, text in m.get("exit_codes", {}).items():
        if int(code) not in codes:
            problems.append("exit_codes: %s is not an exit code app.h names" % code)
        elif (code == "0") != (text == "ok"):
            problems.append("exit_codes: only 0 is 'ok'")
    # 5. the settings
    if mac:
        known, defaults, modes = describe_list(args.describe), {}, []
    else:
        known = settings_list(repo)
        defaults, modes = setting_defaults(repo)
    seen = set()
    for s in m.get("settings", []):
        sid = s["id"]
        if sid in seen:
            problems.append("setting %s is listed twice" % sid)
        seen.add(sid)
        if "in" in s and s["in"] != m["config"][0].get("id", "main"):
            continue
        entries = [(k, v) for c in s.get("choices", []) for k, v in c.get("set", {}).items()]
        entries += list(s.get("defaults", {}).items())
        for key, value in s.get("defaults", {}).items():
            d = known.get(key)
            real = (d["default"] if mac else real_default(d, key, defaults, modes)) if d else None
            if mac and d is None:
                pass
            elif real is not None and norm(real) != norm(value):
                problems.append("%s: default of %s is %s, the game's is %s" % (sid, key, value, real))
            elif d and real is None:
                problems.append("%s: the game's default of %s could not be read from app.h (this check needs updating)" % (sid, key))
        slider = s.get("slider")
        keys = {k for k, _ in entries} | ({slider["key"]} if slider else set())
        for key in sorted(keys):
            d = known.get(key)
            if not d:
                problems.append("%s: %s is not a setting of the game (%s)" % (sid, key, "not in the Mac program's --describe-settings" if mac else "settings_list.cpp"))
                continue
            if "/" not in key or not re.fullmatch(r"\w+/\w+", key):
                problems.append("%s: %s is not Section/Key" % (sid, key))
        for key, value in entries:
            d = known.get(key)
            if not d:
                continue
            problem = value_problem(d, value)
            if problem:
                problems.append("%s: %s = %s: %s" % (sid, key, value, problem))
        if slider:
            d = known.get(slider["key"])
            if d and d["type"] not in ("Int", "Float"):
                problems.append("%s: a slider on %s, which is not a number" % (sid, slider["key"]))
            elif d and d["limits"]:
                lo, hi = d["limits"]
                scale = 100.0 if d["type"] == "Float" else 1.0
                if slider["min"] < lo / scale - 1e-9 or slider["max"] > hi / scale + 1e-9:
                    problems.append("%s: slider %s to %s is outside the game's %s to %s" % (sid, slider["min"], slider["max"], lo / scale, hi / scale))
            if d and d["type"] == "Int" and slider.get("format", "%g") not in ("%d", "%g"):
                problems.append("%s: an integer setting written with %s" % (sid, slider["format"]))
            if d and d["type"] == "Float" and slider.get("format") != "%.2f":
                problems.append("%s: the game writes a float as %%.2f, the slider with %s" % (sid, slider.get("format")))
            if slider["key"] not in s.get("defaults", {}):
                problems.append("%s: no default for %s" % (sid, slider["key"]))
        for c in s.get("choices", []):
            if not c.get("set"):
                problems.append("%s: choice %s writes nothing" % (sid, c["label"]))
        if s.get("choices") and not s.get("defaults"):
            problems.append("%s: no defaults" % sid)
    if mac:
        notes.append("%d settings checked against the Mac program's --describe-settings (%d keys listed)" % (len(m.get("settings", [])), len(known)))
    else:
        notes.append("%d settings checked against sdk/runtime/app/settings_list.cpp and app.h (%d keys known)" % (len(m.get("settings", [])), len(known)))
    notes.append("start: %s %s" % (exe, " ".join(start.get("args", []))))
    if prepare:
        notes.append("prepare: %s %s, done %s, version %s" % (exe, " ".join(prepare.get("args", [])), prepare.get("done"), prepare.get("version")))


def norm(text):
    try:
        return "%.4f" % float(text)
    except ValueError:
        return text


def value_problem(d, value):
    kind = d["type"]
    if kind == "Bool":
        return None if value in ("true", "false") else "a bool is written true or false"
    if kind == "Int":
        if not re.fullmatch(r"-?\d+", value):
            return "an integer is written bare"
        lo, hi = d["limits"] or (None, None)
        if lo is not None and not lo <= int(value) <= hi:
            return "outside %d to %d" % (lo, hi)
        return None
    if kind == "Float":
        if not re.fullmatch(r"-?\d+\.\d+", value):
            return "the game writes a float with a point (1.00)"
        lo, hi = d["limits"] or (None, None)
        if lo is not None and not lo / 100.0 - 1e-9 <= float(value) <= hi / 100.0 + 1e-9:
            return "outside %s to %s" % (lo / 100.0, hi / 100.0)
        return None
    if kind == "Choice":
        if not re.fullmatch(r'"[^"]*"', value):
            return "a choice is written in quotes, as the game writes it"
        if d["choices"] and value[1:-1] not in d["choices"]:
            return "not one of %s" % ", ".join(d["choices"])
        return None
    return "a %s setting is not checked here" % kind


def real_default(d, key, defaults, modes):
    member = d["member"]
    if member is None or member not in defaults:
        return None
    raw = defaults[member]
    kind = d["type"]
    if kind == "Bool":
        return raw
    if kind == "Float":
        return "%.2f" % float(raw.rstrip("f"))
    if kind == "Int":
        return raw
    if kind == "Choice":
        if raw.startswith("DisplayMode::"):
            return '"%s"' % raw.split("::")[1].lower()
        if re.fullmatch(r"\d+", raw):
            choices = d["choices"] or []
            numeric = all(re.fullmatch(r"\d+", c) for c in choices)
            return '"%s"' % (raw if numeric else choices[int(raw)])
    return None


# ---- run ---------------------------------------------------------------------------------------------------------------------------------

def self_test(schema, manifest):
    """files the schema must refuse; True when it refuses every one."""
    v = Validator(schema)
    cases = []
    c = json.loads(json.dumps(manifest))
    c["unknown_key"] = 1
    cases.append(("an unknown top-level key", c))
    c = json.loads(json.dumps(manifest))
    c["format"] = 2
    cases.append(("format 2", c))
    c = json.loads(json.dumps(manifest))
    del c["start"]["program"]
    cases.append(("a start without a program", c))
    c = json.loads(json.dumps(manifest))
    c["settings"][0]["choices"] = c["settings"][0]["choices"][:1]
    cases.append(("a setting with one choice", c))
    c = json.loads(json.dumps(manifest))
    c["system"][next(iter(c["system"]))]["graphics"] = {"glide": "1"}
    cases.append(("an unknown graphics api", c))
    ok = True
    for name, case in cases:
        problems = []
        v.check(case, schema, "", problems)
        print("  self-test, %-28s %s" % (name + ":", "refused (%s)" % problems[0] if problems else "ACCEPTED"))
        ok = ok and bool(problems)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest")
    ap.add_argument("--guide")
    ap.add_argument("--schema")
    ap.add_argument("--no-schema", action="store_true")
    ap.add_argument("--version")
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--describe", help="the Mac program's --describe-settings output (for the Mac's file)")
    args = ap.parse_args()

    try:
        with open(args.manifest, encoding="utf-8") as f:
            manifest = json.load(f)
    except json.JSONDecodeError as e:
        print("%s: not JSON, line %d column %d: %s" % (args.manifest, e.lineno, e.colno, e.msg))
        return 1
    problems, notes = [], []
    schema, source = (None, None) if args.no_schema else load_schema(args)
    if schema:
        found = []
        Validator(schema).check(manifest, schema, "", found)
        problems += ["schema: " + p for p in found]
        notes.append("schema: %s" % source)
        if args.self_test and not self_test(schema, manifest):
            problems.append("the schema accepted a file it must refuse: the check is not working")
    elif args.no_schema:
        print("SCHEMA NOT CHECKED (--no-schema): only the checks against this game ran")
    else:
        print("no schema: give --guide <recomplauncher-agent.md>, --schema <file>, set RECOMPLAUNCHER_GUIDE, or say --no-schema")
        return 2
    game_checks(manifest, args, problems, notes)
    for n in notes:
        print("  " + n)
    if problems:
        print("RECOMPLAUNCHER.JSON FAILED:")
        for p in problems:
            print("  - " + p)
        return 1
    print("RECOMPLAUNCHER.JSON PASSED%s" % ("" if schema else " (schema not checked)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
