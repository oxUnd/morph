"""Exercise ask_user selection and custom answers in the production CLI."""
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal
from test_cli_jobs_pty import until


def run_plain(terminal, releases):
    until(terminal, lambda text: any(row.startswith('> ') for row in text.splitlines()))
    case = 0

    def ask(question, choices=None, **options):
        terminal.send(question + '\r')
        _, release = terminal.request()
        releases.append(release)
        args = {'question': question, **options}
        if choices is not None:
            args['choices'] = choices
        release.delta = {'tool_calls': [{'index': 0, 'id': f'plain-{case}',
            'type': 'function', 'function': {'name': 'ask_user',
            'arguments': json.dumps(args)}}]}
        release.set()
        until(terminal, lambda text: '? ' + question in text)

    def answer(expected, resolution='user'):
        nonlocal case
        body, release = terminal.request()
        releases.append(release)
        results = [json.loads(message['content']) for message in body['messages']
                   if message['role'] == 'tool' and
                   'ask_user_response' in message['content']]
        result = results[-1].get('data', results[-1])
        assert result['answers'] == expected, result
        assert result['resolution'] == resolution, result
        marker = f'PLAIN_CASE_COMPLETE_{case}'
        case += 1
        release.delta = {'content': marker}
        release.set()
        until(terminal, lambda text: marker in text and
              any(row.startswith('> ') for row in text.splitlines()))

    ask('Plain select color', ['Blue', 'Red'])
    terminal.send('\x1b[B\r')
    answer(['Red'])

    ask('Plain long free text')
    until(terminal, lambda text: 'Your answer' in text)
    free_text = '界' * 600 + 'FREE_TEXT_END'
    terminal.send('\x1b[200~' + free_text + '\x1b[201~\r')
    answer([free_text])

    ask('Plain long custom answer', ['Short answer'])
    terminal.send('\x1b[F\r')
    detailed = '界' * 1600 + 'CUSTOM_ANSWER_END'
    before_paste = len(terminal.raw.encode('utf-8'))
    terminal.send('\x1b[200~' + detailed + '\x1b[201~\r')
    answer([detailed])
    paste_output = len(terminal.raw.encode('utf-8')) - before_paste
    assert paste_output < 65536, f'paste caused excessive redraw output: {paste_output}'

    ask('Plain optional timeout', response_policy='optional', timeout_seconds=1)
    answer([], 'timeout_no_input')
    print('PASS: plain CLI select, long free text, bounded paste redraw, and optional timeout')


def main():
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    terminal = None
    releases = []
    try:
        with tempfile.TemporaryDirectory(prefix='morph-select-pty-') as temp:
            terminal = Terminal(Path(sys.argv[1]).resolve(), Path(temp),
                                server.server_port, hooks_enabled=False,
                                extra_config='[agents]\nmax_total_tokens=0\n')
            if '--plain' in sys.argv[2:]:
                run_plain(terminal, releases)
                return
            terminal.wait_for_composer()

            def ask(question, choices=None, **options):
                terminal.send('\x15' + question + '\r')
                _, release = terminal.request()
                releases.append(release)
                terminal.send('preserved draft 中文')
                args = {'question': question, **options}
                if choices is not None:
                    args['choices'] = choices
                release.delta = {'tool_calls': [{'index': 0, 'id': 'select-test',
                    'type': 'function', 'function': {'name': 'ask_user',
                    'arguments': json.dumps(args)}}]}
                release.set()
                until(terminal, lambda text: '? ' + question in text)

            def answer(expected, resolution=None):
                body, release = terminal.request()
                releases.append(release)
                results = [json.loads(m['content']) for m in body['messages']
                           if m['role'] == 'tool' and 'ask_user_response' in m['content']]
                result = results[-1].get('data', results[-1])
                assert result['answers'] == expected, result
                if resolution is not None:
                    assert result['resolution'] == resolution, result
                text = terminal.snapshot('restored composer')
                assert 'preserved draft 中文' in text, text
                assert 'Space toggle' not in text, text
                assert 'Other · Type' not in text, text
                release.delta = {'content': 'Selection received.'}
                release.set()
                until(terminal, lambda text: 'Working ·' not in text)

            for columns in (120, 80, 60):
                terminal.child.setwinsize(24, columns)
                terminal.screen.resize(24, columns)
                terminal.pump(.2)
                ask(f'Pick a color at {columns}', ['Blue', 'Red'])
                text = terminal.snapshot('inline selector')
                assert 'Other · Type your own answer' in text, text
                terminal.send('\x1b[200~j \r\x1b[201~')
                terminal.send('\x1b[B\r')
                answer(['Red'])

            for columns in (120, 80, 60):
                terminal.child.setwinsize(24, columns)
                terminal.screen.resize(24, columns)
                terminal.pump(.2)
                ask(f'Recommended color at {columns}', ['Blue', 'Red'],
                    recommended_indices=[1], response_policy='optional', timeout_seconds=3)
                text = terminal.snapshot('recommended choice preselected')
                assert 'Red (Recommended)' in text, text
                terminal.send('\r')
                answer(['Red'], 'user')

                ask(f'Automatic color at {columns}', ['Blue', 'Red'],
                    recommended_indices=[1], response_policy='optional', timeout_seconds=1)
                answer(['Red'], 'timeout_recommended')

            ask('Editing prevents automatic choice', ['Blue', 'Red'],
                recommended_indices=[1], response_policy='optional', timeout_seconds=2)
            terminal.send('\x1b[A')
            terminal.pump(2.2)
            text = terminal.snapshot('automatic choice paused after navigation')
            assert 'Auto-continue paused' in text, text
            assert Model.requests.empty(), 'model continued before the user submitted'
            terminal.send('\r')
            answer(['Blue'], 'user')

            ask('Recommended pair', ['Blue', 'Red', 'Green'],
                selection_mode='multi', min_choices=2, max_choices=2,
                recommended_indices=[0, 2], response_policy='optional', timeout_seconds=2)
            terminal.send('\r')
            answer(['Blue', 'Green'], 'user')

            ask('Optional free text timeout', response_policy='optional', timeout_seconds=1)
            answer([], 'timeout_no_input')

            ask('Optional free text editing', response_policy='optional', timeout_seconds=2)
            terminal.send('custom')
            terminal.pump(2.2)
            assert Model.requests.empty(), 'custom input lost to auto-continue'
            terminal.send('\r')
            answer(['custom'], 'user')

            ask('Describe another color', ['Blue', 'Red'])
            terminal.send('\x1b[F\r')
            terminal.send('\r')
            assert 'non-empty answer' in terminal.snapshot('empty custom answer rejected')
            terminal.send('discard this\x1b')
            assert 'Other · Type' in terminal.snapshot('editor back returns to list')
            terminal.send('\r')
            terminal.send('\x1b[200~紫色🙂\x1b[201~')
            terminal.send('\x1b[D深\x1b[C\r')
            answer(['紫色深🙂'])

            ask('Choose two colors', ['Blue', 'Red', 'Green'],
                selection_mode='multi', min_choices=2, max_choices=2)
            terminal.send('\r')
            assert 'Selection limit' in terminal.snapshot('minimum enforced')
            terminal.send(' \x1b[B \x1b[B ')
            text = terminal.snapshot('maximum enforced')
            assert '2 selected' in text and 'Selection limit' in text, text
            terminal.send('\x1b[A \x1b[F\r')
            terminal.send('Gold\r')
            assert '2 selected' in terminal.snapshot('custom answer with checked choice')
            terminal.send('e\x15\x1b')
            assert 'Gold' in terminal.snapshot('cancel editing preserves saved answer')
            terminal.send('\r')
            answer(['Blue', 'Gold'])

            ask('A longer list', [f'Choice {i}' for i in range(1, 15)])
            terminal.child.setwinsize(24, 120)
            terminal.screen.resize(24, 120)
            terminal.pump(.2)
            terminal.child.setwinsize(24, 80)
            terminal.screen.resize(24, 80)
            terminal.pump(.2)
            terminal.send('\x1b[6~\x1b[F\x1b[A\r')
            answer(['Choice 14'])

            ask('A free text question')
            terminal.send('My own idea 中文\r')
            answer(['My own idea 中文'])

            ask('A long free text answer')
            free_text = '界' * 600 + 'FREE_TEXT_END'
            terminal.send(free_text + '\r')
            answer([free_text])

            ask('Describe a detailed custom answer', ['Short answer'])
            terminal.send('\x1b[F\r')
            detailed = '界' * 1600 + 'CUSTOM_ANSWER_END'
            before_paste = len(terminal.raw.encode('utf-8'))
            terminal.send('\x1b[200~' + detailed + '\x1b[201~\r')
            answer([detailed])
            paste_output = len(terminal.raw.encode('utf-8')) - before_paste
            assert paste_output < 65536, f'paste caused excessive redraw output: {paste_output}'

            ask('Cancel the selector', ['Yes', 'No'])
            terminal.send('\x03')
            text = until(terminal, lambda text: 'Stopped after' in text)
            assert 'preserved draft 中文' in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            print('PASS: select, limits, pagination, custom UTF-8 answers, paste, '
                  'recommendations, idle fallback, editing pause, draft restoration, plain input, and cancellation')
    finally:
        for release in releases:
            release.set()
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()


if __name__ == '__main__':
    main()
