"""Enforce CLI application module ownership and the frontend/framework dependency boundary."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / 'src/sapi/cli'
ROOT_FILES = {'main.c', 'application.c', 'application.h', 'application_internal.h',
              'CMakeLists.txt', 'README.md'}
APP_DIRS = {'models', 'pages', 'widgets', 'commands', 'services', 'adapters', 'presentation'}
MODULE_NAMES = {'models/composer.c': 'composer_model',
                'adapters/runtime/observer.c': 'runtime_observer'}


def code_only(text):
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*',
                  ' ', text)


def main():
    errors = []
    for entry in CLI.iterdir():
        expected = APP_DIRS if entry.is_dir() else ROOT_FILES
        if entry.name not in expected:
            errors.append(f'{entry}: not part of the application directory contract')
    headers = {}
    owners = {}
    for header in sorted(CLI.rglob('*.h')):
        text = code_only(header.read_text())
        headers[header] = set(re.findall(r'\b(cli_[a-z_0-9]+)\s*\(', text))
        for name in headers[header]:
            if name in owners:
                errors.append(f'{name}: declared by both {owners[name]} and {header}')
            owners[name] = header
        if header.name != 'application_internal.h' and 'sapi/cli/application_internal.h' in header.read_text():
            errors.append(f'{header}: module interface imports the common implementation header')

    cmake = (CLI / 'CMakeLists.txt').read_text()
    private_functions = re.compile(
        r'^static\s+(?:[A-Za-z_]\w*[ \t*]+)+?([A-Za-z_]\w*)'
        r'[ \t]*\([^;{}]*\)\s*\n\{', re.M)
    public_functions = re.compile(
        r'^(?!static\b)(?:[A-Za-z_]\w*[ \t*]+)+?([A-Za-z_]\w*)'
        r'[ \t]*\([^;{}]*\)\s*\n\{', re.M)
    for source in sorted(CLI.rglob('*.c')):
        raw = source.read_text()
        code = code_only(raw)
        header = CLI / 'application.h' if source.name == 'main.c' else source.with_suffix('.h')
        if not header.exists():
            errors.append(f'{source}: missing module interface')
            continue
        own_include = '#include "' + header.relative_to(ROOT / 'src').as_posix() + '"'
        if not raw.startswith(own_include + '\n'):
            errors.append(f'{source}: own interface must be the first include')
        relative = source.relative_to(CLI).as_posix()
        if len(re.findall(r'(?<![\w/])' + re.escape(relative) + r'(?![\w.])', cmake)) != 1:
            errors.append(f'{source}: source must appear exactly once in CMake')
        module = MODULE_NAMES.get(relative, source.stem)
        prefix = 'cli_' + module + '_'
        for name in private_functions.findall(code):
            if not name.startswith(prefix):
                errors.append(f'{source}: private function {name} must start with {prefix}')
        for name in public_functions.findall(code):
            if name != 'main' and name not in headers[header]:
                errors.append(f'{source}: exported function {name} lacks its owner declaration')
        for name in re.findall(
                r'^static\s+(?:[A-Za-z_]\w*[ \t*]+)+?([A-Za-z_]\w*)'
                r'\s*(?:\[[^\]]*\]\s*)?=', code, re.M):
            if not name.startswith(prefix):
                errors.append(f'{source}: private data {name} must start with {prefix}')
        macro_prefix = 'CLI_' + module.upper() + '_'
        for name in re.findall(r'^#define\s+(\w+)', code, re.M):
            if not name.startswith(macro_prefix) or name != name.upper():
                errors.append(f'{source}: private macro {name} must start with {macro_prefix}')
        for kind, name in re.findall(r'\b(struct|union|enum)\s+(\w+)\s*\{', code):
            if name.startswith(prefix) or name == prefix[:-1]:
                continue
            shared = any(re.search(r'\b' + kind + r'\s+' + re.escape(name) + r'\b',
                                   candidate.read_text())
                         for candidate in CLI.rglob('*.h'))
            if not shared:
                errors.append(f'{source}: private {kind} {name} lacks its module prefix')
        includes = set(re.findall(r'^#include "([^"]+)"', raw, re.M))
        for name in set(re.findall(r'\bcli_[a-z_0-9]+\b', code)):
            owner = owners.get(name)
            if owner and owner.relative_to(ROOT / 'src').as_posix() not in includes:
                errors.append(f'{source}: {name} requires its owner interface {owner.name}')
        for target in re.findall(r'^#include [<"]([^>"]+)', raw, re.M):
            if re.match(r'(?:src/)?tui/(?:core|render|terminal|widgets)/', target):
                errors.append(f'{source}: imports framework implementation header {target}')

    internal = code_only((CLI / 'application_internal.h').read_text())
    if re.search(r'\b(?:void|int|char|struct|enum)\s+\**cli_\w+\s*\(', internal):
        errors.append('application_internal.h: function interfaces belong in module headers')
    for source in sorted((CLI / 'commands').glob('*.c')):
        code = code_only(source.read_text())
        if re.search(r'\bmorph_tui_(?:screen_(?:open|run|poll)|application_(?:init|run|step))\s*\(', code):
            errors.append(f'{source}: command handler owns a UI loop')
    app_header = code_only((CLI / 'application.h').read_text())
    if not re.search(r'struct cli_application\s*{\s*struct morph_tui_application base;', app_header):
        errors.append('application.h: Application must embed its TUI base first')
    for field in ('event_stream', 'markdown_stream', 'rendered_artifacts',
                  'completion_sessions', 'readline_ready_input', 'prompt_lock'):
        if re.search(r'\b' + field + r'\b', app_header):
            errors.append(f'application.h: {field} belongs to its presentation/input owner')
    queue = (CLI / 'services/event_queue.c').read_text()
    if re.search(r'^#include [<"](?:readline/|sapi/cli/(?:pages|widgets|presentation|adapters)/)',
                 queue, re.M):
        errors.append('services/event_queue.c: transport imports UI dispatch or presentation')
    transcript = (CLI / 'widgets/transcript.c').read_text()
    if '\\033[?1049' in transcript or 'previous_frame' in transcript:
        errors.append('widgets/transcript.c: modal terminal/frame ownership belongs to the page')
    if re.search(r'\b(?:morph_array_push_borrow|utf8_terminal_sanitize_feed|patch_preview)\s*\(',
                 code_only(transcript)):
        errors.append('widgets/transcript.c: state reduction and workspace reads belong outside rendering')
    if '#include "sapi/cli/application_internal.h"' in transcript:
        errors.append('widgets/transcript.c: rendering must import its actual interfaces')
    model = (CLI / 'models/transcript.c').read_text()
    if re.search(r'^#include [<"](?:agent/patch|util/file|sapi/cli/presentation)', model, re.M):
        errors.append('models/transcript.c: projection updates must not render or read workspace files')
    feed = code_only((CLI / 'widgets/feed.c').read_text())
    if re.search(r'\b(?:ioctl|TIOCGWINSZ|winsize)\b', feed):
        errors.append('widgets/feed.c: terminal dimensions belong to the shared size helper')
    chat = code_only((CLI / 'pages/chat.c').read_text())
    if 'morph_tui_view_controller_present_borrow' not in chat:
        errors.append('pages/chat.c: chat must present a retained ViewController')
    if re.search(r'\b(?:fprintf|printf|fputs|fwrite|putchar|morph_tui_frame_present|'
                 r'cli_readline_(?:render_frame|suspend_copy|resume_take))\s*\(', chat):
        errors.append('pages/chat.c: retained chat must not own terminal painting or Readline geometry')
    if re.search(r'\b(?:morph_tui_view_get_options|'
                 r'morph_tui_canvas_(?:text|fill|child)\w*)\s*\(', chat):
        errors.append('pages/chat.c: margin and padding painting belongs to framework views')
    for module in ('message', 'tool_call'):
        component = code_only((CLI / f'widgets/{module}.c').read_text())
        apis = ['morph_tui_widget_add_borrow', 'morph_tui_widget_text_view_add_borrow']
        if module == 'message':
            apis.append('morph_tui_widget_label_add_borrow')
        for api in apis:
            if api not in component:
                errors.append(f'widgets/{module}.c: domain component must compose base controls ({api})')
    for source in sorted(CLI.rglob('*')):
        if source.suffix not in ('.c', '.h'):
            continue
        raw = source.read_text()
        code = code_only(raw)
        relative = source.relative_to(CLI).as_posix()
        for target in re.findall(r'^#include [<"]([^>"]+)', raw, re.M):
            if target.startswith(('sapi/cli/ui/', 'sapi/cli/setup/')):
                errors.append(f'{source}: obsolete application include {target}')
            if relative.startswith('models/') and target.startswith((
                    'sapi/cli/pages/', 'sapi/cli/widgets/', 'sapi/cli/adapters/',
                    'sapi/cli/services/', 'sapi/cli/application', 'runtime/')):
                errors.append(f'{source}: model depends on application behavior {target}')
            if relative.startswith('services/configuration/') and target.startswith((
                    'sapi/cli/pages/', 'sapi/cli/widgets/')):
                errors.append(f'{source}: configuration service depends on a page {target}')
        if relative != 'application.h' and re.search(r'\bstruct morph_tui_application\s+\w+\s*;', code):
            errors.append(f'{source}: second TUI Application instance outside application.h')
        if relative != 'application.c' and re.search(
                r'\bmorph_tui_application_(?:init|run|deinit)\s*\(', code):
            errors.append(f'{source}: Application host lifecycle belongs in application.c')
    # Application status shapes have one shared owner. Content bullets and
    # framework radio controls are deliberately outside this application audit.
    status_glyphs = set('●◉◯○•◦⊗⦻ⓧ✖✘✗✓✔❌❓⨂⍰⯑⚠◇')
    for source in sorted(CLI.rglob('*')):
        if source.suffix not in ('.c', '.h'):
            continue
        raw = re.sub(r'/\*[\s\S]*?\*/', '', source.read_text())
        for literal in re.findall(r'"(?:\\.|[^"\\])*"', raw):
            if status_glyphs.intersection(literal):
                errors.append(f'{source}: hardcoded status glyph; use presentation/style.h')
    style = (CLI / 'presentation/style.h').read_text()
    canonical = (ROOT / 'src/tui/include/morph/tui/terminal/status.h').read_text()
    for state, glyph in {'PENDING': '◯', 'RUNNING': '◉', 'DONE': '●',
                         'FAILED': '✖', 'QUESTION': '?', 'CANCELLED': '–',
                         'WARNING': '⚠', 'SYSTEM': '◇'}.items():
        if f'#define MORPH_TERMINAL_STATUS_{state} "{glyph}"' not in canonical:
            errors.append(f'terminal_status.h: canonical {state} marker changed')
        if f'#define CLI_FEED_{state} MORPH_TERMINAL_STATUS_{state}' not in style:
            errors.append(f'presentation/style.h: {state} must reuse the shared definition')
    for state, color in {'FAILED': 'RED', 'QUESTION': 'YELLOW'}.items():
        if f'#define CLI_FEED_{state}_STYLE ANSI_BOLD ANSI_{color}' not in style:
            errors.append(f'presentation/style.h: {state} must use bold {color.lower()}')
    for source in (ROOT / 'src/tui').rglob('*'):
        if source.suffix not in ('.c', '.h'):
            continue
        if re.search(r'^#include [<"](?:sapi/cli|runtime|agent|db|config)/',
                     source.read_text(), re.M):
            errors.append(f'{source}: framework imports a frontend/domain module')
    if errors:
        raise AssertionError('\n'.join(errors))
    print(f'PASS: {len(list(CLI.rglob("*.c")))} application modules own their interfaces, '
          'private names, source registration, and dependencies')


if __name__ == '__main__':
    main()
