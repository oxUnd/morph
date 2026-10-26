"""History/context inspector through the production Readline loop and local SSE."""
import http.server
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
from test_cli_transcript_pty import AlternateScreen
import pyte


def seed(database):
    with sqlite3.connect(database) as db:
        session = db.execute("SELECT id FROM sessions WHERE name='history-view'").fetchone()[0]
        db.execute('DELETE FROM model_history_items WHERE session_id=?', (session,))
        db.execute('DELETE FROM messages WHERE session_id=?', (session,))
        for sequence in range(1, 301):
            kind = 'user_message' if sequence % 2 else 'assistant_message'
            role = 'user' if sequence % 2 else 'assistant'
            content = f'HISTORY_ROW_{sequence:03d}'
            payload = None
            tool_name = None
            call_id = None
            provider_id = None
            active = sequence >= 281
            trimmed = False
            if sequence in (280, 281):
                kind, role, content = 'compaction_summary', 'system', 'Condensed build decisions'
            if sequence == 283:
                kind, role = 'assistant_tool_calls', 'assistant'
                payload = json.dumps({'calls': [{'tool_call_id': 'history-call',
                    'provider_call_id': 'provider-call', 'name': 'exec', 'input_kind': 'json',
                    'arguments': json.dumps({'command': 'cmake --build build'})}]})
            if sequence == 284:
                kind, role, tool_name = 'tool_result', 'tool', 'exec'
                call_id, provider_id = 'history-call', 'provider-call'
                content, trimmed = 'BUILD_SHORTENED_RESULT\n... truncated ...', True
            if sequence == 300:
                content = '中文 target latest'
            if sequence == 299:
                content = 'unsafe \x1b]52;c;NEVER_EXECUTE\x07 text'
            db.execute('INSERT INTO model_history_items(session_id,sequence_no,turn_id,kind,role,'
                       'content,payload_json,tool_call_id,provider_call_id,tool_name,token_count,'
                       'active,truncated,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                       (session, sequence, 'history-turn', kind, role, content, payload,
                        call_id, provider_id, tool_name, 7, active, trimmed, sequence))
        ids = dict(db.execute('SELECT sequence_no,id FROM model_history_items WHERE session_id=?',
                              (session,)))
        db.executemany('INSERT INTO history_summary_sources VALUES(?,?)',
                       [(ids[280], ids[index]) for index in range(1, 201)] +
                       [(ids[281], ids[280])])


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='morph-history-pty-') as temp:
        terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
        release = None
        try:
            terminal.child.setwinsize(32, 100)
            terminal.screen = AlternateScreen(100, 32)
            terminal.stream = pyte.Stream(terminal.screen)
            deadline = time.monotonic() + 8
            while 'ctrl+o details' not in terminal.raw:
                assert time.monotonic() < deadline, terminal.raw[-3000:]
                terminal.pump(.05)
            terminal.send('/new history-view\r')
            deadline = time.monotonic() + 8
            while 'created and switched to session: history-view' not in terminal.raw:
                assert time.monotonic() < deadline, terminal.raw[-3000:]
                terminal.pump(.05)
            terminal.send('snapshot question\r')
            _, release = terminal.request()
            release.delta = {'content': 'HISTORY_MODEL_REPLY'}
            release.set()
            deadline = time.monotonic() + 8
            while 'Worked for' not in terminal.raw:
                assert time.monotonic() < deadline, terminal.raw[-3000:]
                terminal.pump(.05)
            with sqlite3.connect(Path(temp) / 'data.db') as db:
                snapshot = db.execute('SELECT input_json FROM model_request_snapshots').fetchone()[0]
                assert 'snapshot question' in snapshot and 'tools' in snapshot, snapshot
            seed(Path(temp) / 'data.db')
            terminal.send('/history\r')
            overview = terminal.snapshot('full history with current membership')
            assert '300 saved entries' in overview, overview
            assert 'estimated history' in overview, overview
            assert '#300' in overview and '中文 target latest' in overview, overview
            assert '\x1b[?1049h' in terminal.raw, 'viewer must use a temporary screen'
            unchanged = terminal.raw
            terminal.pump(.4)
            assert terminal.raw == unchanged, 'idle inspector must not repaint'
            terminal.send('/中文 target\r')
            searched = terminal.snapshot('Unicode search')
            assert '1 matching' in searched and '#300' in searched, searched
            terminal.send('\r')
            detail = terminal.snapshot('estimated request detail')
            assert 'Estimated request view' in detail and '中文 target latest' in detail, detail
            terminal.send('v')
            assert 'Stored view' in terminal.snapshot('stored detail')
            terminal.send('\x1b')
            terminal.send('/\x15tool\r')
            tools = terminal.snapshot('tool call and result search')
            assert '2 matching' in tools and '#283' in tools and '#284' in tools, tools
            terminal.send('\x1b[B\r')
            result = terminal.snapshot('trimmed result disclosure')
            assert 'IN / TRIMMED' in result and 'original is unavailable' in result, result
            terminal.send('v')
            assert 'BUILD_SHORTENED_RESULT' in terminal.snapshot('stored trimmed output')
            terminal.send('s')
            request = terminal.snapshot('last submitted model input')
            assert 'Last submitted model input' in request and 'pty-test' in request, request
            assert 'headers and generation options omitted' in request, request
            terminal.send('\x1b')
            terminal.send('/\x15\r\t\t')
            summaries = terminal.snapshot('summary ancestry filter')
            assert '[Summarized]' in summaries and 'VIA SUMMARY' in summaries, summaries
            assert '#201' not in summaries, summaries
            terminal.send('\t')
            excluded = terminal.snapshot('excluded history filter')
            assert '[Excluded]' in excluded and '#201' in excluded, excluded
            terminal.send('\t/\x15中文 target\r\r')
            terminal.child.setwinsize(24, 40)
            terminal.screen.resize(24, 40)
            terminal.pump(.3)
            narrow = terminal.snapshot('narrow detail keeps exit hint')
            assert 'Esc back' in narrow, narrow
            terminal.child.setwinsize(24, 20)
            terminal.screen.resize(24, 20)
            terminal.pump(.3)
            assert 'Widen terminal' in terminal.snapshot('too small is recoverable')
            terminal.child.setwinsize(32, 100)
            terminal.screen.resize(32, 100)
            terminal.pump(.3)
            widened = terminal.snapshot('detail restored after resize')
            assert '#300' in widened and '中文 target latest' in widened, widened
            terminal.send('\x1b')
            terminal.send('\x1b[200~ssqqjj\x1b[201~')
            assert 'morph / history' in terminal.snapshot('paste cannot activate commands')
            terminal.send('\x1b')
            restored = terminal.snapshot('main scrollback restored')
            assert 'HISTORY_MODEL_REPLY' in restored, restored
            assert 'morph / history' not in restored, restored
            terminal.send('AFTER_VIEW_DRAFT')
            assert 'AFTER_VIEW_DRAFT' in terminal.snapshot('composer accepts input after viewer')
            terminal.send('\x15/history\r')
            assert 'morph / history' in terminal.snapshot('viewer reopened before interrupt')
            terminal.send('\x03')
            interrupted = terminal.snapshot('Ctrl-C closes viewer and restores input')
            assert 'morph / history' not in interrupted, interrupted
            assert '\x1b[3J' not in terminal.raw, 'must not erase scrollback'
            assert '\x1b]52;c;' not in terminal.raw, 'must not emit untrusted terminal escapes'
            with sqlite3.connect(Path(temp) / 'data.db') as db:
                assert db.execute('SELECT count(*) FROM model_history_items').fetchone()[0] == 300
            print('PASS: history inspection, request snapshot, filters, Unicode, details, resize, cleanup')
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)
            server.shutdown()


if __name__ == '__main__':
    main()
