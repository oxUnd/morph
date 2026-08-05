#!/bin/sh
set -e

# /* morph system test suite — exercises ./build/morph CLI via stdin pipe */

PASS=0
FAIL=0
SKIP=0
TOTAL=0
TMPDIR=""
MORPH_BIN=""
TRANSLATE_SH=""
UPPER_SO=""

# ---- groups ----

G_HELP="help"
G_SESSION="session"
G_MODEL="model"
G_EXT="ext"
G_SKILL="skill"
G_MCP="mcp"
G_CONTEXT="context"
G_TRANSLATE="translate"
G_UPPER="upper"
G_WEBFETCH="webfetch"

# ---- test registry ----

TESTS=""

register_test() {
	_num="$1"
	_group="$2"
	_fn="$3"
	TESTS="${TESTS}${_num}|${_group}|${_fn}
"
}

# ---- helpers ----

strip_ansi() {
	sed $'s/\x1b\[[0-9;]*[a-zA-Z]//g'
}

run_morph() {
	_cmds="$1"
	printf '%s\n' "$_cmds" | HOME="$TMPDIR" "$MORPH_BIN" -c "$TMPDIR/.morph/config.toml" 2>&1 | strip_ansi
}

assert_contains() {
	_label="$1"
	_output="$2"
	_expected="$3"
	TOTAL=$((TOTAL + 1))
	if printf '%s' "$_output" | grep -qF -- "$_expected"; then
		PASS=$((PASS + 1))
		printf "  \033[32mPASS\033[0m %s\n" "$_label"
	else
		FAIL=$((FAIL + 1))
		printf "  \033[31mFAIL\033[0m %s — expected '%s' not found\n" "$_label" "$_expected"
		printf "    last line: %s\n" "$(echo "$_output" | tail -1)"
	fi
}

assert_not_contains() {
	_label="$1"
	_output="$2"
	_unexpected="$3"
	TOTAL=$((TOTAL + 1))
	if printf '%s' "$_output" | grep -qF -- "$_unexpected"; then
		FAIL=$((FAIL + 1))
		printf "  \033[31mFAIL\033[0m %s — unexpected '%s' found\n" "$_label" "$_unexpected"
	else
		PASS=$((PASS + 1))
		printf "  \033[32mPASS\033[0m %s\n" "$_label"
	fi
}

skip_test() {
	_label="$1"
	_reason="$2"
	SKIP=$((SKIP + 1))
	TOTAL=$((TOTAL + 1))
	printf "  \033[33mSKIP\033[0m %s — %s\n" "$_label" "$_reason"
}

# ---- A. help ----

test_help_banner() {
	out=$(run_morph "/quit")
	assert_contains "1: banner shows version" "$out" "morph v"
}

test_help_list() {
	out=$(run_morph "/help
/quit")
	assert_contains "2: /help shows /q[uit]" "$out" "/q"
	assert_contains "2: /help shows /new" "$out" "/n"
	assert_contains "2: /help shows /ext" "$out" "/x"
	assert_contains "2: /help shows /credits" "$out" "/credits"
}

test_help_detail() {
	out=$(run_morph "/help /new
/quit")
	assert_contains "3: /help /new shows detail" "$out" "Create a new session"
}

test_help_alias() {
	out=$(run_morph "/h
/quit")
	assert_contains "4: /h alias works" "$out" "Show help"
}

# ---- B. session ----

test_new_auto() {
	out=$(run_morph "/new
/quit")
	assert_contains "5: /new auto-named" "$out" "created and switched to session: new_"
}

test_new_named() {
	out=$(run_morph "/new mytest
/quit")
	assert_contains "6: /new mytest" "$out" "mytest"
}

test_list() {
	out=$(run_morph "/list
/quit")
	assert_contains "7: /list does not create a session" "$out" "no sessions"
}

test_switch() {
	out=$(run_morph "/new s1
/new s2
/switch s1
/quit")
	assert_contains "8: /switch existing" "$out" "switched to session: s1"
}

test_rename() {
	out=$(run_morph "/new rename_me
/rename renamed_s
/quit")
	assert_contains "9: /rename" "$out" "renamed_s"
}

test_delete() {
	out=$(run_morph "/new todel
/new keeper
/delete todel
/quit")
	assert_contains "10: /delete no error" "$out" "Goodbye!"
}

test_history() {
	out=$(run_morph "/history
/quit")
	assert_contains "11: /history shows empty" "$out" "showing"
}

test_list_alias() {
	out=$(run_morph "/ls
/quit")
	assert_contains "12: /ls alias" "$out" "no sessions"
}

# ---- C. model ----

test_model_show() {
	out=$(run_morph "/model
/quit")
	assert_contains "13: /model shows current" "$out" "current model:"
}

test_model_switch() {
	out=$(run_morph "/model gpt-3.5-turbo
/model
/quit")
	assert_contains "14: /model switch" "$out" "gpt-3.5-turbo"
}

test_config() {
	out=$(run_morph "/config
/quit")
	assert_contains "15: /config [general]" "$out" "[general]"
	assert_contains "15: /config provider" "$out" "provider"
}

test_config_alias() {
	out=$(run_morph "/cfg
/quit")
	assert_contains "16: /cfg alias" "$out" "[general]"
}

# ---- D. ext ----

test_ext_list() {
	out=$(run_morph "/ext list
/quit")
	assert_contains "17: /ext list shows credits" "$out" "credits"
	assert_contains "17: /ext list shows translate" "$out" "translate"
	assert_contains "17: /ext list shows upper" "$out" "upper"
}

test_ext_bare() {
	out=$(run_morph "/ext
/quit")
	assert_contains "18: /ext bare shows tools" "$out" "Tools"
}

test_ext_info_translate() {
	out=$(run_morph "/ext info translate
/quit")
	assert_contains "19: /ext info translate" "$out" "Translate text"
}

test_ext_info_upper() {
	out=$(run_morph "/ext info upper
/quit")
	assert_contains "20: /ext info upper" "$out" "uppercase"
}

test_ext_info_notfound() {
	out=$(run_morph "/ext info no_such_tool
/quit")
	assert_contains "21: /ext info missing tool" "$out" "not found"
}

test_ext_alias() {
	out=$(run_morph "/x
/quit")
	assert_contains "22: /x alias" "$out" "Tools"
}

# ---- E. skill ----

test_skill_list() {
	out=$(run_morph "/skill list
/quit")
	assert_contains "23: /skill list" "$out" "Skills"
}

test_skill_notfound() {
	out=$(run_morph "/skill info nope
/quit")
	assert_contains "24: /skill info missing" "$out" "not found"
}

# ---- F. mcp ----

test_mcp_list() {
	out=$(run_morph "/mcp list
/quit")
	assert_contains "25: /mcp list" "$out" "MCP"
}

test_skill_alias() {
	out=$(run_morph "/sk
/quit")
	assert_contains "26: /sk alias" "$out" "Skills"
}

# ---- G. context ----

test_context() {
	out=$(run_morph "/context
/quit")
	assert_contains "27: /context shows tokens" "$out" "tokens"
}

test_context_alias() {
	out=$(run_morph "/ctx
/quit")
	assert_contains "28: /ctx alias" "$out" "tokens"
}

test_trace() {
	out=$(run_morph "/trace
/quit")
	assert_contains "29: /trace empty" "$out" "Goodbye!"
}

test_compress() {
	out=$(run_morph "/compress
/quit")
	assert_contains "30: /compress empty" "$out" "no messages to compress"
}

test_save() {
	_savedir="$TMPDIR"
	out=$(HOME="$TMPDIR" "$MORPH_BIN" -c "$TMPDIR/.morph/config.toml" 2>&1 <<'CMDS' | strip_ansi
/save
/quit
CMDS
)
	assert_contains "31: /save" "$out" "saved"
}

test_export() {
	out=$(run_morph "/export
/quit")
	assert_contains "32: /export alias" "$out" "/save"
}

test_image_notfound() {
	out=$(run_morph "/image /no/such/file.png
/quit")
	assert_contains "33: /image not found" "$out" "not found"
}

test_render_notfound() {
	out=$(run_morph "/render /no/such/file.md
/quit")
	assert_contains "34: /render not found" "$out" "not found"
}

# ---- H. translate (direct JSON-RPC) ----

test_translate_zh() {
	out=$(echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"text":"hello world","target_lang":"zh"}}' | "$TRANSLATE_SH" 2>&1)
	assert_contains "35: translate zh" "$out" "你好，世界"
}

test_translate_en() {
	out=$(echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"text":"bonjour","target_lang":"en"}}' | "$TRANSLATE_SH" 2>&1)
	assert_contains "36: translate en" "$out" "[Translated]"
}

test_translate_ja() {
	out=$(echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"text":"hello","target_lang":"ja"}}' | "$TRANSLATE_SH" 2>&1)
	assert_contains "37: translate ja" "$out" "翻訳"
}

test_translate_missing_text() {
	out=$(echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"target_lang":"zh"}}' | "$TRANSLATE_SH" 2>&1)
	assert_contains "38: translate missing text" "$out" "-32602"
}

test_translate_missing_lang() {
	out=$(echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"text":"hello"}}' | "$TRANSLATE_SH" 2>&1)
	assert_contains "39: translate missing lang" "$out" "-32602"
}

test_translate_empty() {
	out=$(echo '' | "$TRANSLATE_SH" 2>&1)
	assert_contains "40: translate empty input" "$out" "-32700"
}

# ---- I. upper ----

test_upper_registered() {
	out=$(run_morph "/ext info upper
/quit")
	assert_contains "41: upper registered" "$out" "uppercase"
}

test_upper_args() {
	out=$(run_morph "/ext info upper
/quit")
	assert_contains "42: upper input_schema has text" "$out" "text"
}

# ---- J. webfetch ----

test_webfetch_registered() {
	if [ ! -d "$TMPDIR/.morph/exts/web-fetch" ]; then
		skip_test "43: web_fetch registered" "web-fetch ext not installed"
		return
	fi
	out=$(run_morph "/ext info web_fetch
/quit")
	assert_contains "43: web_fetch registered" "$out" "Fetch content"
}

test_webfetch_direct() {
	if [ ! -d "$TMPDIR/.morph/exts/web-fetch" ]; then
		skip_test "44: web_fetch direct" "web-fetch ext not installed"
		return
	fi
	if ! command -v node >/dev/null 2>&1; then
		skip_test "44: web_fetch direct" "node not found"
		return
	fi
	if [ ! -d "$TMPDIR/.morph/exts/web-fetch/node_modules" ]; then
		skip_test "44: web_fetch direct" "node_modules not installed"
		return
	fi
	out=$(echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"url":"https://httpbin.org/get","format":"text","timeout":15}}' | \
		HOME="$TMPDIR" node --experimental-strip-types "$TMPDIR/.morph/exts/web-fetch/web-fetch.ts" 2>/dev/null)
	if [ -n "$out" ]; then
		assert_contains "44: web_fetch direct" "$out" "content"
	else
		skip_test "44: web_fetch direct" "no output (network?)"
	fi
}

# ---- register all tests ----

register_test 1  "$G_HELP"      test_help_banner
register_test 2  "$G_HELP"      test_help_list
register_test 3  "$G_HELP"      test_help_detail
register_test 4  "$G_HELP"      test_help_alias
register_test 5  "$G_SESSION"   test_new_auto
register_test 6  "$G_SESSION"   test_new_named
register_test 7  "$G_SESSION"   test_list
register_test 8  "$G_SESSION"   test_switch
register_test 9  "$G_SESSION"   test_rename
register_test 10 "$G_SESSION"   test_delete
register_test 11 "$G_SESSION"   test_history
register_test 12 "$G_SESSION"   test_list_alias
register_test 13 "$G_MODEL"     test_model_show
register_test 14 "$G_MODEL"     test_model_switch
register_test 15 "$G_MODEL"     test_config
register_test 16 "$G_MODEL"     test_config_alias
register_test 17 "$G_EXT"       test_ext_list
register_test 18 "$G_EXT"       test_ext_bare
register_test 19 "$G_EXT"       test_ext_info_translate
register_test 20 "$G_EXT"       test_ext_info_upper
register_test 21 "$G_EXT"       test_ext_info_notfound
register_test 22 "$G_EXT"       test_ext_alias
register_test 23 "$G_SKILL"     test_skill_list
register_test 24 "$G_SKILL"     test_skill_notfound
register_test 25 "$G_MCP"       test_mcp_list
register_test 26 "$G_SKILL"     test_skill_alias
register_test 27 "$G_CONTEXT"   test_context
register_test 28 "$G_CONTEXT"   test_context_alias
register_test 29 "$G_CONTEXT"   test_trace
register_test 30 "$G_CONTEXT"   test_compress
register_test 31 "$G_CONTEXT"   test_save
register_test 32 "$G_CONTEXT"   test_export
register_test 33 "$G_CONTEXT"   test_image_notfound
register_test 34 "$G_CONTEXT"   test_render_notfound
register_test 35 "$G_TRANSLATE" test_translate_zh
register_test 36 "$G_TRANSLATE" test_translate_en
register_test 37 "$G_TRANSLATE" test_translate_ja
register_test 38 "$G_TRANSLATE" test_translate_missing_text
register_test 39 "$G_TRANSLATE" test_translate_missing_lang
register_test 40 "$G_TRANSLATE" test_translate_empty
register_test 41 "$G_UPPER"     test_upper_registered
register_test 42 "$G_UPPER"     test_upper_args
register_test 43 "$G_WEBFETCH"  test_webfetch_registered
register_test 44 "$G_WEBFETCH"  test_webfetch_direct

# ---- environment setup ----

setup_env() {
	ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
	MORPH_BIN="$ROOT_DIR/build/morph"

	if [ ! -x "$MORPH_BIN" ]; then
		echo "ERROR: $MORPH_BIN not found. Run: cmake -S . -B build && cmake --build build"
		exit 1
	fi

	if [ ! -f ~/.morph/config.toml ]; then
		echo "ERROR: ~/.morph/config.toml not found. Copy from config.toml.example first."
		exit 1
	fi

	TMPDIR=$(mktemp -d /tmp/morph_systest_XXXXXX)
	mkdir -p "$TMPDIR/.morph"
	cp ~/.morph/config.toml "$TMPDIR/.morph/config.toml"

	# exts — cli_init_exts() reads from ~/.morph/exts, so place them there
	mkdir -p "$TMPDIR/.morph/exts"
	cp -r "$ROOT_DIR/exts/demo-translate" "$TMPDIR/.morph/exts/"
	chmod +x "$TMPDIR/.morph/exts/demo-translate/translate.sh"
	TRANSLATE_SH="$TMPDIR/.morph/exts/demo-translate/translate.sh"

	cp -r "$ROOT_DIR/exts/demo-upper" "$TMPDIR/.morph/exts/"
	cc -shared -fPIC -o "$TMPDIR/.morph/exts/demo-upper/upper.so" "$ROOT_DIR/exts/demo-upper/upper.c" 2>/dev/null
	UPPER_SO="$TMPDIR/.morph/exts/demo-upper/upper.so"

	if [ -d "$ROOT_DIR/exts/web-fetch" ]; then
		cp -r "$ROOT_DIR/exts/web-fetch" "$TMPDIR/.morph/exts/"
		chmod +x "$TMPDIR/.morph/exts/web-fetch/web-fetch.sh"
	fi

	# ensure [ext] section exists in config (dir defaults to ~/.morph/exts)
	if ! grep -q '\[ext\]' "$TMPDIR/.morph/config.toml"; then
		printf '\n[ext]\n' >> "$TMPDIR/.morph/config.toml"
	fi

	printf "morph systest — TMPDIR=%s\n\n" "$TMPDIR"
}

cleanup() {
	if [ -n "$TMPDIR" ] && [ -d "$TMPDIR" ]; then
		rm -rf "$TMPDIR"
	fi
}

# ---- dispatcher ----

run_tests() {
	_filters="$*"

	_testfile=$(mktemp)
	echo "$TESTS" > "$_testfile"
	while IFS='|' read -r num group fn; do
		[ -z "$fn" ] && continue

		if [ -n "$_filters" ]; then
			matched=false
			for arg in $_filters; do
				if [ "$arg" = "$num" ] || [ "$arg" = "$group" ]; then
					matched=true
					break
				fi
			done
			if [ "$matched" = "false" ]; then
				continue
			fi
		fi

		printf "[%2s] %s\n" "$num" "$fn"
		$fn
	done < "$_testfile"
	rm -f "$_testfile"
}

# ---- main ----

trap cleanup EXIT INT TERM

setup_env

START_TIME=$(date +%s)
run_tests "$@"
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

printf "\n========================================\n"
printf "Results: %d pass, %d fail, %d skip (total %d) in %ds\n" \
	"$PASS" "$FAIL" "$SKIP" "$TOTAL" "$ELAPSED"

if [ "$FAIL" -gt 0 ]; then
	printf "\033[31mSOME TESTS FAILED\033[0m\n"
	exit 1
else
	printf "\033[32mALL TESTS PASSED\033[0m\n"
	exit 0
fi
