"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the ten GLSL pieces of `source/Shaders.cpp` (kQuadVertex,
kCommon, kSignal and the seven pass bodies: wall, copy, route, hold, stats,
panel, display) plus kVersion. That is two copies of the same text, and two
copies drift -- quietly, because a wall whose zebra takes the wrong partner row,
or whose repeat runs against the cable, still looks like a faulty LED wall. The
whole claim of the page is that it runs the plugin's own shaders, so the claim
needs something enforcing it. `pwtest` drives the real plugin class and has
never heard of this page, and `tools/glslc.sh` compiles the C++ copies and never
looks at the JS one.

The plugin does not run any piece on its own: `Assemble()` builds each pass's
fragment shader as kVersion + kCommon (+ kSignal) + the pass, and InitGL builds
the vertex stage as kVersion + kQuadVertex. A page that ran the right pieces in
the wrong order, or left kSignal out of the stats pass, would not be running
the plugin's shaders either. So the ORDER is copied and checked too: this
script reads `Assemble()`'s switch out of Shaders.cpp (and the vertex line out
of Patchwork.cpp's InitGL), writes it into shaders.js as `ASSEMBLY`, and the
page builds every program from that table and nothing else.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of `source/Shaders.cpp` and the matching
backtick literal out of `demo/shaders.js`, and compares them exactly -- no
whitespace normalisation, no comment stripping: a comment updated on one side
only is exactly the drift worth catching. `kVersion`, a plain string, is
compared too. Then:

  - every `const char* const k... = R"(` piece in Shaders.cpp must be one this
    script knows, so a new piece cannot be added to the plugin and silently
    left out of the page;
  - every value of `enum class Pass` in Shaders.h must have a case in
    `Assemble()`, and ASSEMBLY must list exactly those passes, piece for piece,
    in the plugin's order;
  - and finally shaders.js must be byte for byte what `--write` would produce,
    so nothing else can be typed into it either.

The one transformation is a decode, not a normalisation: a comment in kCommon
quotes `at` in backticks, and a backtick cannot appear raw inside a template
literal, so shaders.js escapes it as \\`. This undoes that and REJECTS any other
backslash or any `${` on the JS side; there are none in the C++ bodies, so
either could only be somebody hiding a difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half. The conversions, `ThresholdU32`, the
layout, the cable, the repeat's motion (`RepeatMotion`, Random's PCG), the time
slots (`SlotOf`), the ring, the ping-pongs and their validity flags and the heat
alpha in demo/plugin.js are a hand translation of source/Controls.cpp,
Wall.cpp, Hash.h and Patchwork.cpp's ProcessOpenGL, and only a reader can tell
whether they still agree. When you change one of those, change it there too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there. kVersion is a plain string and is handled on its own.
SHADERS = [
    ("QUAD_VERTEX", "kQuadVertex"),
    ("COMMON", "kCommon"),
    ("SIGNAL", "kSignal"),
    ("WALL_FRAGMENT", "kWallFragment"),
    ("COPY_FRAGMENT", "kCopyFragment"),
    ("ROUTE_FRAGMENT", "kRouteFragment"),
    ("HOLD_FRAGMENT", "kHoldFragment"),
    ("STATS_FRAGMENT", "kStatsFragment"),
    ("PANEL_FRAGMENT", "kPanelFragment"),
    ("DISPLAY_FRAGMENT", "kDisplayFragment"),
]
JS_NAME = dict((symbol, name) for name, symbol in SHADERS)
JS_NAME["kVersion"] = "VERSION"

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape is \\` for a backtick inside a comment.
"""


class Stale(Exception):
    """The C++ is not shaped the way this script reads it: fail, never guess."""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def cpp_version(source):
    match = re.search(r'const char\* const kVersion = "(.*?)";', source)
    return None if match is None else match.group(1)


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def unknown_pieces(cpp):
    """Raw-string pieces in Shaders.cpp that this script does not copy."""
    declared = re.findall(r'const char\* const (k\w+) = R"\(', cpp)
    return [symbol for symbol in declared if symbol not in JS_NAME]


def assembly(cpp, plugin_cpp, shaders_h):
    """The plugin's own assembly: the vertex stage from InitGL, every pass from
    Assemble()'s switch, the passes from the Pass enum. In JS names."""
    enum = re.search(r"enum class Pass\s*\{(.*?)\};", shaders_h, re.S)
    if enum is None:
        raise Stale("no `enum class Pass` in source/Shaders.h")
    passes = [p.strip() for p in enum.group(1).split(",") if p.strip()]

    body = re.search(r"std::string Assemble\( Pass pass \)\s*\{(.*?)\n\}", cpp, re.S)
    if body is None:
        raise Stale("no Assemble( Pass pass ) in source/Shaders.cpp")
    if body.group(1).count("switch") != 1:
        raise Stale("Assemble() is not one switch")
    before, switch = body.group(1).split("switch", 1)
    preamble = re.findall(r"(?:std::string out = |out \+= )(k\w+);", before)
    if not preamble:
        raise Stale("Assemble() starts from no piece")
    cases = re.findall(r"case Pass::(\w+):(.*?)break;", switch, re.S)

    vertex = re.search(r"const std::string vertex = (.*?);", plugin_cpp)
    if vertex is None:
        raise Stale("no `const std::string vertex = ...;` in Patchwork.cpp's InitGL")

    table = [("vertex", re.findall(r"shaders::(k\w+)", vertex.group(1)))]
    seen = set()
    for name, statements in cases:
        seen.add(name)
        table.append((name.lower(), preamble + re.findall(r"out \+= (k\w+);", statements)))
    missing = [p for p in passes if p not in seen]
    extra = sorted(seen - set(passes))
    if missing or extra:
        raise Stale(f"Assemble()'s cases ({sorted(seen)}) are not the Pass enum ({passes})")

    out = []
    for name, symbols in table:
        for symbol in symbols:
            if symbol not in JS_NAME:
                raise Stale(f"{name} uses {symbol}, a piece this script does not copy")
        out.append((name, [JS_NAME[s] for s in symbols]))
    return out


def render(cpp, plugin_cpp, shaders_h):
    """shaders.js as --write produces it."""
    version = cpp_version(cpp)
    if version is None:
        raise Stale("kVersion not found in source/Shaders.cpp")
    out = [HEADER]
    out.append(f"export const VERSION = '{version}';\n")
    for name, symbol in SHADERS:
        body = from_cpp(cpp, symbol)
        if body is None:
            raise Stale(f"{symbol} not found in source/Shaders.cpp")
        if "\\" in body or "${" in body:
            raise Stale(f"{symbol} holds a backslash or ${{; the escape scheme cannot carry it")
        out.append(f"\n// {symbol}, source/Shaders.cpp\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    out.append(
        "\n// Each stage's source, piece by piece, in the order the plugin joins them:\n"
        "// the vertex stage as Patchwork.cpp's InitGL builds it, every fragment\n"
        "// shader as Shaders.cpp's Assemble() does.\n"
        "export const ASSEMBLY = {\n"
    )
    for name, pieces in assembly(cpp, plugin_cpp, shaders_h):
        out.append(f"  {name}: [{', '.join(repr(p) for p in pieces)}],\n")
    out.append("};\n")
    return "".join(out)


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(cpp, plugin_cpp, shaders_h, js):
    problems = 0

    for symbol in unknown_pieces(cpp):
        print(f"FAIL  {symbol} is a piece of source/Shaders.cpp this script does not copy -- add it to SHADERS")
        problems += 1

    version_cpp = cpp_version(cpp)
    version_js = re.search(r"^export const VERSION = '(.*?)';$", js, re.M)
    if version_cpp is None or version_js is None or version_cpp != version_js.group(1):
        print("FAIL  VERSION does not match kVersion")
        problems += 1
    else:
        print(f"ok    {'VERSION':<20} matches kVersion")

    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    try:
        table = assembly(cpp, plugin_cpp, shaders_h)
    except Stale as stale:
        print(f"FAIL  the plugin's assembly could not be read: {stale}")
        return problems + 1
    block = re.search(r"^export const ASSEMBLY = \{\n(.*?)^\};$", js, re.S | re.M)
    rows = {} if block is None else dict(re.findall(r"^  (\w+): \[(.*)\],$", block.group(1), re.M))
    for name, pieces in table:
        want = ", ".join(repr(p) for p in pieces)
        if rows.get(name) == want:
            print(f"ok    {'ASSEMBLY.' + name:<20} {' + '.join(pieces)}")
        else:
            print(f"FAIL  ASSEMBLY.{name} is [{rows.get(name, '<missing>')}], the plugin's is [{want}]")
            problems += 1
    for name in sorted(set(rows) - set(n for n, _ in table)):
        print(f"FAIL  ASSEMBLY.{name} is no stage of the plugin's")
        problems += 1

    # Anything else typed into the file.
    try:
        expected = render(cpp, plugin_cpp, shaders_h)
    except Stale as stale:
        print(f"FAIL  {stale}")
        return problems + 1
    if expected != js:
        where = first_difference(expected, js)
        print("FAIL  demo/shaders.js is not exactly what --write produces")
        if where:
            print(f"        first difference at line {where[0]}")
            print(f"          want: {where[1]}")
            print(f"          have: {where[2]}")
        problems += 1
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    plugin_cpp = read("source", "Patchwork.cpp")
    shaders_h = read("source", "Shaders.h")
    if "--write" in argv:
        unknown = unknown_pieces(cpp)
        if unknown:
            print(f"FAIL  {', '.join(unknown)}: piece(s) this script does not copy -- add them to SHADERS")
            return 1
        try:
            text = render(cpp, plugin_cpp, shaders_h)
        except Stale as stale:
            print(f"FAIL  {stale}")
            return 1
        with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
            handle.write(text)
        print("wrote demo/shaders.js")
        return 0
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(cpp, plugin_cpp, shaders_h, js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write, do not edit shaders.js by hand")
        return 1
    stages = len(assembly(cpp, plugin_cpp, shaders_h))
    print(f"all {len(SHADERS) + 1} shader pieces, and the order all {stages} stages join them in, are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
