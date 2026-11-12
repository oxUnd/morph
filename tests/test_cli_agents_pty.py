"""Real collaboration activity, running /agents and composer redraws in a PTY."""
import http.server
import json
from pathlib import Path
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, terminal_environment
from test_cli_transcript_pty import AlternateScreen
from test_cli_jobs_pty import StableComposerStream
from cli_inline_test_support import inline_panel, stable_rows
import pyte


def wait_for(terminal, predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        terminal.pump(.05)
        if predicate():
            return
    raise AssertionError(terminal.raw[-8000:])


def tool_delta(name, arguments, call_id):
    return {'tool_calls': [{'index': 0, 'id': call_id, 'type': 'function',
                           'function': {'name': name, 'arguments': json.dumps(arguments)}}]}


def run(driver, port, columns):
    releases = []
    with tempfile.TemporaryDirectory(prefix='morph-agents-pty-') as temp:
        terminal = Terminal(driver, Path(temp), port, hooks_enabled=False,
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n')
        terminal.screen = AlternateScreen(80, 24)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            terminal.child.setwinsize(24, columns)
            terminal.screen.resize(24, columns)
            terminal.send('/agents\r')
            wait_for(terminal, lambda: 'Agents (0)' in '\n'.join(terminal.screen.display))
            assert hasattr(terminal.screen, '_main_screen')
            assert 'Main conversation' in '\n'.join(stable_rows(terminal))
            terminal.send('\x1b')
            wait_for(terminal, lambda: not hasattr(terminal.screen, '_main_screen'))
            terminal.send('exercise collaboration\r')
            _, root = terminal.request()
            releases.append(root)
            wait_for(terminal, lambda: any('Working' in line and 'Agents: 0 running' in line
                                         for line in terminal.screen.display))
            working = next(line for line in terminal.screen.display if 'Working' in line)
            assert 'Agents: 0 running' in working, '\n'.join(terminal.screen.display)
            assert not any(line.lstrip().startswith('Agents:') for line in terminal.screen.display)
            root.delta = tool_delta('spawn_agent', {'task_name': 'worker',
                'message': 'CHILD_ACTIVITY_TASK: inspect independently', 'fork_turns': 'none'}, 'spawn_work')
            root.set()
            pair = [terminal.request(), terminal.request()]
            releases.extend(release for _, release in pair)
            child = next(release for body, release in pair
                         if any('You are `/root/worker`' in message.get('content', '')
                                for message in body.get('messages', []) if message['role'] == 'system'))
            primary = next(release for _, release in pair if release is not child)
            wait_for(terminal, lambda: 'Agents: 1 running' in '\n'.join(terminal.screen.display))
            wait_for(terminal, lambda: any('└ ◉ /root/worker' in line for line in terminal.screen.display))
            active_row = next(line for line in terminal.screen.display if '└ ◉ /root/worker' in line)
            assert 'CHILD_ACTIVITY_TASK' not in active_row
            working = next(line for line in terminal.screen.display if 'Working' in line)
            assert 'Agents: 1 running' in working, '\n'.join(terminal.screen.display)
            assert not any(line.lstrip().startswith('Agents:') for line in terminal.screen.display)
            primary.delta = tool_delta('wait_agent', {'timeout_ms': 10000}, 'wait_work')
            primary.set()
            wait_for(terminal, lambda: 'Waiting for agents' in '\n'.join(terminal.screen.display))
            assert 'Running…' not in '\n'.join(terminal.screen.display)
            child.delta = tool_delta('exec', {'command': 'sleep 2'}, 'child_inspect')
            child.set()
            wait_for(terminal, lambda: 'Running exec' in re.sub(r'\x1b\[[0-9;]*m', '', terminal.raw))
            for _ in range(4):
                terminal.pump(.3)
                visible = '\n'.join(stable_rows(terminal))
                assert visible.count('Waiting for agents') == 1, visible
            terminal.send('/agents\r')
            wait_for(terminal, lambda: 'Agents (1)' in terminal.screen.display[0])
            assert hasattr(terminal.screen, '_main_screen')
            assert 'Esc' in terminal.screen.display[-1]
            assert 'exercise collaboration' not in '\n'.join(terminal.screen.display)
            terminal.send('\t\t\t')
            wait_for(terminal, lambda: 'No matching agents.' in '\n'.join(terminal.screen.display))
            terminal.send('\r')
            terminal.pump(.2)
            assert hasattr(terminal.screen, '_main_screen'), 'empty filter opened a conversation'
            terminal.send('\x1b[Z\x1b[Z\x1b[Z')
            wait_for(terminal, lambda: 'Main conversation' in '\n'.join(terminal.screen.display))
            terminal.send('\x1b[B')
            wait_for(terminal, lambda: 'CHILD_ACTIVITY_TASK' in '\n'.join(terminal.screen.display))
            terminal.send('\x1b[C\x1b[6~')
            wait_for(terminal, lambda: 'scroll details' in terminal.screen.display[-1])
            assert any(row.startswith('> ') and '/root/worker' in row
                       for row in terminal.screen.display)
            terminal.send('\x1b[D?')
            wait_for(terminal, lambda: 'Shift+Tab' in terminal.screen.display[-1])
            terminal.send('?')
            for width in (60, 120, 80, columns):
                terminal.child.setwinsize(24, width)
                terminal.screen.resize(lines=24, columns=width)
                terminal.pump(.2)
                rows = stable_rows(terminal)
                assert 'Agents (1)' in rows[0], '\n'.join(rows)
                selected = next(i for i, row in enumerate(rows) if row.startswith('> '))
                assert '/root/worker' in rows[selected]
                selected_end = (width - 1) * 3 // 5 - 1 if width >= 101 else width - 2
                assert terminal.screen.buffer[selected][selected_end].bg == 'blue'
                if width >= 101:
                    assert 'Task details' in '\n'.join(rows), '\n'.join(rows)
                    assert terminal.screen.buffer[selected][width - 2].bg != 'blue'
                assert 'Esc' in rows[-1]
            for height in (8, 4, 40, 24):
                terminal.child.setwinsize(height, columns)
                terminal.screen.resize(lines=height, columns=columns)
                terminal.pump(.2)
                rows = stable_rows(terminal)
                assert 'Agents (1)' in rows[0], '\n'.join(rows)
                assert any(row.startswith('> ') and '/root/worker' in row for row in rows), '\n'.join(rows)
                assert 'Esc' in rows[-1], '\n'.join(rows)
            assert not any(line.startswith('» /agents') for line in terminal.screen.display)
            switches = terminal.raw.count('\x1b[?1049h')
            terminal.send('\r')
            wait_for(terminal, lambda: 'Agents · /root/worker' in terminal.screen.display[0])
            assert terminal.raw.count('\x1b[?1049h') == switches, 'nested alternate screen'
            terminal.send('\x1b')
            wait_for(terminal, lambda: not hasattr(terminal.screen, '_main_screen'))
            terminal.stream = StableComposerStream(terminal)
            terminal.send('keep my draft')
            terminal.pump(.2)
            _, child_done = terminal.request()
            releases.append(child_done)
            child_done.delta = {'content': 'CHILD_VERIFIED_RESULT'}
            child_done.set()
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                terminal.pump(.05)
                try:
                    _, release = Model.requests.get_nowait()
                    releases.append(release)
                    release.delta = {'content': 'ROOT_INTEGRATED_RESULT'}
                    release.set()
                except queue.Empty:
                    pass
                if 'Finished /root/worker' in re.sub(r'\x1b\[[0-9;]*m', '', terminal.raw) and 'ROOT_INTEGRATED_RESULT' in terminal.raw:
                    break
            else:
                raise AssertionError(terminal.raw[-8000:])
            terminal.pump(.5)
            visible = '\n'.join(stable_rows(terminal))
            assert visible.count('keep my draft') == 1, visible
            terminal.child.send('\x15')
            terminal.send('/subagents\r')
            wait_for(terminal, lambda: 'Main conversation' in '\n'.join(terminal.screen.display))
            assert not hasattr(terminal.screen, '_main_screen'), 'subagents became full-screen'
            inline_panel(terminal, 'Subagents', 'ROOT_INTEGRATED_RESULT',
                         maximum=12, command='/subagents')
            terminal.send('\x1b')
            wait_for(terminal, lambda: not any('Subagents' in row
                     for row in terminal.screen.display))
            terminal.send('/agents /root/worker\r')
            wait_for(terminal, lambda: 'Agents · /root/worker' in terminal.screen.display[0])
            assert 'CHILD_VERIFIED_RESULT' in '\n'.join(stable_rows(terminal))
            assert hasattr(terminal.screen, '_main_screen')
            terminal.send('\x1b')
            wait_for(terminal, lambda: not hasattr(terminal.screen, '_main_screen'))
            assert sum(line.startswith('» ') for line in terminal.screen.display) == 1
            if columns == 120:
                plain = subprocess.run(
                    [str(driver.with_name('morph')), '-c', str(Path(temp) / 'config.toml'),
                     '-w', temp, '--events', 'json'], input='/agents\n/quit\n',
                    env=terminal_environment(Path(temp)), text=True, capture_output=True,
                    timeout=20)
                assert plain.returncode == 0, plain.stderr
                events = [json.loads(line) for line in plain.stdout.splitlines()]
                assert any('Agents (' in event.get('data', {}).get('output', '')
                           for event in events), plain.stdout
                assert '\x1b' not in plain.stdout
            print(f'checked agent activity, inspection and draft at {columns} columns', flush=True)
        except Exception:
            print('\n'.join(terminal.screen.display), flush=True)
            raise
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            while not Model.requests.empty():
                _, release = Model.requests.get_nowait()
                release.set()


def run_automatic_wait(driver, port):
    releases = []
    with tempfile.TemporaryDirectory(prefix='morph-agent-wait-pty-') as temp:
        terminal = Terminal(driver, Path(temp), port, hooks_enabled=False,
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n')
        try:
            terminal.wait_for_composer()
            terminal.send('collect worker results\r')
            _, root = terminal.request()
            releases.append(root)
            root.delta = tool_delta('spawn_agent', {'task_name': 'worker',
                'message': 'Inspect independently', 'fork_turns': 'none'}, 'spawn_wait')
            root.set()
            pair = [terminal.request(), terminal.request()]
            releases.extend(release for _, release in pair)
            child = next(release for body, release in pair
                         if any('You are `/root/worker`' in message.get('content', '')
                                for message in body.get('messages', [])
                                if message['role'] == 'system'))
            primary = next(release for _, release in pair if release is not child)
            primary.delta = {'content': 'PARENT_PROPOSAL_PENDING_WORKER'}
            primary.set()
            wait_for(terminal, lambda: 'PARENT_PROPOSAL_PENDING_WORKER' in terminal.raw)
            for _ in range(4):
                terminal.pump(.3)
                visible = '\n'.join(stable_rows(terminal))
                assert visible.count('Waiting for agents') == 1, visible
                assert 'Agents: 1 running' in visible, visible
            child.delta = {'content': 'WORKER_REPORT_COMPLETE'}
            child.set()
            _, final = terminal.request()
            releases.append(final)
            final.delta = {'content': 'PARENT_REPORT_COMPLETE'}
            final.set()
            wait_for(terminal, lambda: 'PARENT_REPORT_COMPLETE' in terminal.raw)
            terminal.pump(.3)
            assert 'Waiting for agents' not in '\n'.join(terminal.screen.display)
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            while not Model.requests.empty():
                _, release = Model.requests.get_nowait()
                release.set()


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        run_automatic_wait(driver, server.server_port)
        for columns in (120, 80, 60):
            run(driver, server.server_port, columns)
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
