#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# CI's raster, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught).
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|int partner = r ^ ( 1 << k );|int partner = r ^ ( 2 << k );|--zebra|GLSL: the zebra's partner row one address bit too high, 1 -> 2"
	"source/Shaders.cpp|int e       = dir > 0 ? a : L - 1 - a;|int e       = dir < 0 ? a : L - 1 - a;|--repeat|GLSL: the repeat's along-index runs against the cable, > -> <"
	"source/Shaders.cpp|bool downstream = ( Hooks & 16 ) != 0 ? ln.pos <= at : ln.pos >= at;|bool downstream = ( Hooks & 16 ) != 0 ? ln.pos <= at : ln.pos <= at;|--chain|GLSL: a break loses the tiles before it, >= -> <="
	"source/Shaders.cpp|float heat  = prev.r + ( P - prev.r ) * HeatAlpha;|float heat  = prev.r + ( P - prev.g ) * HeatAlpha;|--heat|GLSL: the heat relaxes towards the hiccup timer, r -> g"
	"source/Shaders.cpp|fragColor = n > 0 ? sum / float( n ) : vec4( 0.0 );|fragColor = n > 0 ? sum * float( n ) : vec4( 0.0 );|--identity|GLSL: the LED's box sum is not divided, / -> *"
	"source/Shaders.cpp|power = ( Hooks & 128 ) != 0 ? sqrt( PsuLimit / state.b ) : PsuLimit / state.b;|power = ( Hooks & 128 ) != 0 ? sqrt( PsuLimit / state.b ) : PsuLimit * state.b;|--psu|GLSL: Dim multiplies by the drive, / -> *"
	"source/Wall.cpp|case Route::RowSnake: return y * l.cols + ( ( y & 1 ) == 0 ? x : l.cols - 1 - x );|case Route::RowSnake: return y * l.cols + ( ( y & 1 ) != 0 ? x : l.cols - 1 - x );|--chain-law|C++: the row snake starts its first row backwards, == -> !="
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target pwtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	extra=()
	[[ "$check" == "--chain-law" ]] || extra=( --size 320x180 )
	limit 300 "$tree/build/pwtest" "$check" "${extra[@]}" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		grep -E '^  FAIL' "$WORK/log" | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
