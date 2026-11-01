"""Enforce CLI UI module ownership and the frontend/framework dependency boundary."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / 'src/sapi/cli'
UI = CLI / 'ui'


def code_only(text):
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*',
                  ' ', text)


def main():
    errors = []
    headers = {}
    owners = {}
    for header in sorted(UI.rglob('*.h')):
        text = code_only(header.read_text())
        headers[header] = set(re.findall(r'\b(cli_[a-z_0-9]+)\s*\(', text))
        for name in headers[header]:
            if name in owners:
                errors.append(f'{name}: declared by both {owners[name]} and {header}')
            owners[name] = header
        if 'sapi/cli/internal.h' in header.read_text():
            errors.append(f'{header}: module interface imports the common implementation header')

    cmake = (CLI / 'CMakeLists.txt').read_text()
    private_functions = re.compile(
        r'^static\s+(?:[A-Za-z_]\w*[ \t*]+)+?([A-Za-z_]\w*)'
        r'[ \t]*\([^;{}]*\)\s*\n\{', re.M)
    public_functions = re.compile(
        r'^(?!static\b)(?:[A-Za-z_]\w*[ \t*]+)+?([A-Za-z_]\w*)'
        r'[ \t]*\([^;{}]*\)\s*\n\{', re.M)
    for source in sorted(UI.rglob('*.c')):
        raw = source.read_text()
        code = code_only(raw)
        header = source.with_suffix('.h')
        if not header.exists():
            errors.append(f'{source}: missing module interface')
            continue
        own_include = '#include "' + header.relative_to(ROOT / 'src').as_posix() + '"'
        if not raw.startswith(own_include + '\n'):
            errors.append(f'{source}: own interface must be the first include')
        relative = source.relative_to(CLI).as_posix()
        if len(re.findall(r'^\s*' + re.escape(relative) + r'\s*$', cmake, re.M)) != 1:
            errors.append(f'{source}: source must appear exactly once in CMake')
        prefix = 'cli_' + source.stem + '_'
        for name in private_functions.findall(code):
            if not name.startswith(prefix):
                errors.append(f'{source}: private function {name} must start with {prefix}')
        for name in public_functions.findall(code):
            if name not in headers[header]:
                errors.append(f'{source}: exported function {name} lacks its owner declaration')
        macro_prefix = 'CLI_' + source.stem.upper() + '_'
        for name in re.findall(r'^#define\s+(\w+)', code, re.M):
            if not name.startswith(macro_prefix):
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

    internal = code_only((CLI / 'internal.h').read_text())
    if re.search(r'\b(?:void|int|char|struct|enum)\s+\**cli_\w+\s*\(', internal):
        errors.append('internal.h: function interfaces belong in module headers')
    for source in sorted((CLI / 'commands').glob('*.c')):
        code = code_only(source.read_text())
        if re.search(r'\bmorph_tui_(?:screen_(?:open|run|poll)|application_(?:run|step))\s*\(', code):
            errors.append(f'{source}: command handler owns a UI loop')
    run = code_only((CLI / 'run.c').read_text())
    if re.search(r'\brl_\w+|\bcli_application_root\b', run):
        errors.append('run.c: Readline and Application implementations belong in ui/')
    for source in (ROOT / 'src/tui').rglob('*'):
        if source.suffix not in ('.c', '.h'):
            continue
        if re.search(r'^#include [<"](?:sapi/cli|runtime|agent|db|config)/',
                     source.read_text(), re.M):
            errors.append(f'{source}: framework imports a frontend/domain module')
    if errors:
        raise AssertionError('\n'.join(errors))
    print(f'PASS: {len(list(UI.rglob("*.c")))} UI modules own their interfaces, '
          'private names, source registration, and dependencies')


if __name__ == '__main__':
    main()
