"""Real composer, commands, streaming steering and persisted preference QA."""
import http.server
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal
from test_cli_transcript_pty import AlternateScreen
import pyte


def open_memory_section(terminal, index):
    terminal.send('\x1b[H' + '\x1b[B' * index + '\r')


def check_memory_panel(terminal, directory):
    with sqlite3.connect(directory / 'data.db') as db:
        session = db.execute('SELECT session_id FROM memory_scopes LIMIT 1').fetchone()[0]
        db.executemany('INSERT INTO memory_candidates(session_id,kind,key,value,source) '
                       'VALUES(?,?,?,?,?)', [
            (session, 'project', 'composer_layout', 'Composer uses explicit insets',
             'Layout evidence \x1b]52;c;MEMORY_UNSAFE\x07 verified'),
            (session, 'lesson', 'composer_testing', 'Verify composer at narrow terminal widths',
             'All layout tests passed'),
            (session, 'preference', 'language', 'Unconfirmed candidate', 'Inferred source'),
        ])
    for width in (120, 80, 60):
        terminal.child.setwinsize(30, width)
        terminal.screen = AlternateScreen(width, 30)
        terminal.stream = pyte.Stream(terminal.screen)
        terminal.send('/memory\r')
        menu = terminal.snapshot(f'memory list at {width} columns')
        assert 'morph / memory' in menu and 'morph / memory /' not in menu, menu
        assert 'Saved facts (1)' in menu and 'Reusable tips (1)' in menu, menu
        assert 'Enter open' in menu and '1-8' not in menu, menu
        assert 'Memory status, counts and settings.' in menu, menu
        terminal.send('3')
        assert 'morph / memory /' not in terminal.snapshot('digits do not open sections')
        terminal.send('\r')
        overview = terminal.snapshot('Enter opens the selected overview')
        assert 'morph / memory / Summary' in overview, overview
        assert 'Saved facts: 1' in overview and 'Reusable tips: 1' in overview, overview
        assert 'Saving past work: enabled' in overview, overview
        terminal.send('\x1b')
        open_memory_section(terminal, 2)
        knowledge = terminal.snapshot('knowledge with source and session')
        assert 'Composer uses explicit insets' in knowledge, knowledge
        assert 'Source:' in knowledge and 'Session:' in knowledge, knowledge
        assert 'Facts remembered about you and this project.' in knowledge, knowledge
        assert '\x1b]52;c;MEMORY_UNSAFE' not in terminal.raw
        terminal.send('\x1b')
        returned = terminal.snapshot('Esc returns to the section list')
        assert 'morph / memory /' not in returned and 'Saved facts (1)' in returned, returned
        terminal.send('\x1b[B')
        hint = terminal.snapshot('selection explains what reusable tips contain')
        assert 'Methods and mistakes learned from past work.' in hint, hint
        terminal.send('\x1b[A\r')
        assert 'morph / memory / Saved facts' in terminal.snapshot('section selection is retained')
        terminal.send('/composer\r')
        assert 'Composer uses explicit insets' in terminal.snapshot('ranked panel search')
        terminal.send('/\x15不存在的关键词\r')
        assert 'No matching memories' in terminal.snapshot('Chinese query with no results')
        terminal.send('/\x15\r')
        assert 'Composer uses explicit insets' in terminal.snapshot('clear search')
        terminal.send('\x1b')
        open_memory_section(terminal, 3)
        assert 'Verify composer' in terminal.snapshot('evidence backed lessons')
        terminal.send('\x1b')
        open_memory_section(terminal, 5)
        assert 'Unconfirmed candidate' in terminal.snapshot('candidate isolation')
        terminal.send('\x1b')
        open_memory_section(terminal, 6)
        assert 'Recent memory updates' in terminal.snapshot('background memory jobs')
        terminal.send('\x1b')
        open_memory_section(terminal, 7)
        assert 'Memory prepared for the most recent reply' in terminal.snapshot('memory context inspection')
        terminal.send('/')
        terminal.send('\x1b')
        assert 'morph / memory' in terminal.snapshot('cancel panel search')
        terminal.send('\x1b')
        menu = terminal.snapshot('context returns to section list')
        assert 'Enter open' in menu and 'morph / memory /' not in menu, menu
        terminal.send('/composer\r')
        menu = terminal.snapshot('search and refresh preserve the selected list row')
        assert 'Saved facts (1 matching)' in menu, menu
        terminal.send('r\r')
        assert 'Memory prepared for the most recent reply' in terminal.snapshot('refresh preserves Context selection')
        terminal.send('\x1b')
        terminal.send('\x1b')
        closed = terminal.snapshot('memory panel returns to composer')
        assert 'morph / memory /' not in closed, closed
        assert not any(row.startswith('› /memory') for row in closed.splitlines()), closed
        assert not terminal.duplicate_prompts
        terminal.send('\x1b[A')
        assert '/memory' in terminal.snapshot('memory command remains in recall history')
        terminal.send('\x15')
    terminal.child.setwinsize(24, 80)
    terminal.screen.resize(lines=24, columns=80)
    terminal.send('/memory show\r')
    assert 'Effective preferences' in terminal.snapshot('text memory inspection remains available')
    terminal.send('/memory retry\r')
    assert 'Memory update retry requested.' in terminal.snapshot('retry memory updates')


def system_prompt(body):
    return '\n'.join(message.get('content', '') for message in body['messages']
                     if message['role'] in ('system', 'developer'))


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    releases = []
    terminal = None
    with tempfile.TemporaryDirectory(prefix='morph-memory-pty-') as temp:
        directory = Path(temp)
        try:
            terminal = Terminal(driver, directory, server.server_port, memory_enabled=True,
                                memory_save_tasks=True)
            terminal.pump(1)
            terminal.send('/memory set language Chinese\r')
            assert 'Preference saved' in terminal.raw, terminal.raw[-4000:]
            assert 'response.language: Chinese [personal' in terminal.raw
            terminal.snapshot('preference command committed')
            check_memory_panel(terminal, directory)
            terminal.send('检查项目\r')
            first, release = terminal.request()
            releases.append(release)
            assert 'response.language: Chinese [personal' in system_prompt(first)
            terminal.send('以后都用英文回答，默认简洁\r')
            # Enter queues steering; Esc applies it to the active turn.
            terminal.child.send('\x1b')
            second, release = terminal.request()
            releases.append(release)
            assert '以后都用英文回答，默认简洁' in json.dumps(second, ensure_ascii=False), second
            assert 'response.language: Chinese' in system_prompt(second), second
            release.delta = {'tool_calls': [
                {'index': 0, 'id': 'language-preference', 'type': 'function', 'function': {
                    'name': 'memory_preference', 'arguments': json.dumps({
                        'key': 'response.language', 'value': 'English', 'scope': 'personal',
                        'evidence': '以后都用英文回答，默认简洁'}, ensure_ascii=False)}},
                {'index': 1, 'id': 'detail-preference', 'type': 'function', 'function': {
                    'name': 'memory_preference', 'arguments': json.dumps({
                        'key': 'response.detail', 'value': 'concise', 'scope': 'personal',
                        'evidence': '以后都用英文回答，默认简洁'}, ensure_ascii=False)}},
            ]}
            release.set()
            after_save, release = terminal.request()
            releases.append(release)
            assert 'response.language: English [personal' in system_prompt(after_save), after_save
            assert 'response.language: Chinese' not in system_prompt(after_save)
            assert 'response.detail: concise' in system_prompt(after_save)
            assert any(message['role'] == 'tool' and 'saved' in message.get('content', '')
                       for message in after_save['messages']), after_save
            terminal.snapshot('steering updates next model request')
            for event in releases:
                event.set()
            terminal.pump(1)
            terminal.send('/memory\r')
            open_memory_section(terminal, 7)
            context = terminal.snapshot('assembled context reflects steering preference')
            assert 'response.language: English' in context, context
            terminal.send('\x1b')
            terminal.send('\x1b')
            terminal.send('/memory history\r')
            assert 'Preference history and sources' in terminal.raw
            terminal.snapshot('history and effective scope')
            terminal.send('/new memory-context-other\r')
            terminal.send('/memory\r')
            open_memory_section(terminal, 7)
            isolated = terminal.snapshot('new session cannot display a previous context')
            assert 'No reply memory has been prepared' in isolated, isolated
            terminal.send('\x1b')
            terminal.send('\x1b')
            terminal.child.setwinsize(24, 48)
            terminal.pump()
            terminal.snapshot('narrow terminal memory view')
            terminal.child.setwinsize(24, 80)
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
            assert not terminal.duplicate_prompts

            terminal = Terminal(driver, directory, server.server_port, memory_enabled=True)
            terminal.pump(1)
            terminal.send('这次用中文回答\r')
            temporary, release = terminal.request()
            releases.append(release)
            assert 'response.language: English [personal' in system_prompt(temporary)
            release.set()
            terminal.pump(1)
            terminal.send('继续工作\r')
            following, release = terminal.request()
            releases.append(release)
            assert 'response.language: English [personal' in system_prompt(following)
            release.set()
            terminal.pump(1)
            terminal.snapshot('restart inheritance and temporary request isolation')
            terminal.send('/memory set language Chinese session\r')
            assert 'response.language: Chinese [session' in terminal.raw
            terminal.send('/memory unset language session\r')
            terminal.send('/memory explain\r')
            terminal.snapshot('unset restores personal preference')
            terminal.send('/memory unset language\r')
            terminal.send('检查删除结果\r')
            cleared, release = terminal.request()
            releases.append(release)
            assert 'response.language:' not in system_prompt(cleared)
            release.set()
            terminal.pump(1)
            assert not terminal.duplicate_prompts
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
            print('PASS: memory commands, streaming steering, restart, temporary scope, unset')
        finally:
            for event in releases:
                event.set()
            if terminal and terminal.child.isalive():
                terminal.child.close(force=True)
            server.shutdown()


if __name__ == '__main__':
    main()
