"""Reproducible multi-agent task evaluation and read-only session analysis.

Scripted runs test the real CLI, filesystem edits, scheduling and independent C
acceptance tests. They do not measure model intelligence. --provider-config opts
into live inference using that profile's credentials from the current process.
No credentials, source code, prompts or complete model replies enter the report.
"""
import argparse
import collections
import contextlib
import http.server
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import subprocess
import tempfile
import threading
import time
import tomllib

from eval_multi_agents_acceptance import capture_integrity, verify_task

HEADER = '''#ifndef EVAL_TASK_H
#define EVAL_TASK_H
#include <stddef.h>
int clamp_add(int a, int b);
size_t ascii_upper_count(const char *data, size_t length);
int combined_score(int a, int b, const char *data, size_t length);
#endif
'''
MATH = '''#include "task.h"
#include <limits.h>
int clamp_add(int a, int b)
{
    if (b > 0 && a > INT_MAX - b) return INT_MAX;
    if (b < 0 && a < INT_MIN - b) return INT_MIN;
    return a + b;
}
'''
TEXT = '''#include "task.h"
size_t ascii_upper_count(const char *data, size_t length)
{
    size_t count = 0;
    if (!data) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 'A' && c <= 'Z') ++count;
    }
    return count;
}
'''
REPORT = '''#include "task.h"
#include <limits.h>
int combined_score(int a, int b, const char *data, size_t length)
{
    size_t count = ascii_upper_count(data, length);
    int extra = count > (size_t)INT_MAX ? INT_MAX : (int)count;
    return clamp_add(clamp_add(a, b), extra);
}
'''
VERIFY = '''#include "task.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
int main(void)
{
    const char bytes[] = {'A', 0, 'Z', (char)255, 'a'};
    assert(clamp_add(2, 3) == 5);
    assert(clamp_add(INT_MAX, 1) == INT_MAX);
    assert(clamp_add(INT_MIN, -1) == INT_MIN);
    assert(clamp_add(INT_MIN, INT_MAX) == -1);
    assert(clamp_add(INT_MAX, -INT_MAX) == 0);
    assert(ascii_upper_count(bytes, sizeof(bytes)) == 2);
    assert(ascii_upper_count(NULL, 9) == 0);
    assert(ascii_upper_count("AzBY", 0) == 0);
    assert(ascii_upper_count("AzBY", 3) == 2);
    assert(combined_score(3, 4, "AzB", 3) == 9);
    assert(combined_score(INT_MAX, 1, "AZ", 2) == INT_MAX);
    assert(combined_score(INT_MIN, 0, "AZ", 2) == INT_MIN + 2);
    puts("ACCEPTANCE_OK: 12 assertions");
    return 0;
}
'''
SOURCES = {'math': MATH, 'text': TEXT, 'report': REPORT}
VERIFY_COMMAND = ('cc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude '
                  'src/math.c src/text.c src/report.c verify.c -o verify && ./verify')


def _eval_json_object(value):
    if isinstance(value, dict):
        return value
    try:
        parsed = json.loads(value or '{}')
    except (ValueError, TypeError):
        return {}
    return parsed if isinstance(parsed, dict) else {}


def _eval_has_table(db, name):
    return db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
                      (name,)).fetchone() is not None


def _eval_tool_receipt(row):
    metadata = _eval_json_object(row['payload_json'])
    original = metadata.get('original_content', row['content'])
    receipt = _eval_json_object(original)
    data = receipt.get('data', receipt)
    if not isinstance(data, dict):
        data = {}
    successful = (metadata.get('status') == 'completed' and
                  metadata.get('error_code') == 0 and receipt.get('ok') is not False)
    return metadata, data, successful


def _eval_patch_contract(call):
    text = call.get('input', call.get('arguments', ''))
    arguments = _eval_json_object(text)
    if arguments:
        text = arguments.get('patch', arguments.get('input', ''))
    if not isinstance(text, str):
        return False
    additions = []
    header_section = False
    for line in text.splitlines():
        section = re.match(r'\*\*\* (?:Add|Update|Delete) File: (.+)$', line)
        if section:
            header_section = _eval_task_path(section.group(1).strip(), 'include/task.h')
        elif line.startswith('*** '):
            header_section = False
        elif header_section and line.startswith('+') and not line.startswith('+++'):
            additions.append(line[1:])
    additions = '\n'.join(additions)
    return all(re.search(r'\b' + name + r'\s*\([^;{}]*\)\s*;', additions)
               for name in ('clamp_add', 'ascii_upper_count'))


def _eval_task_path(path, relative):
    if not isinstance(path, str):
        return False
    return path == relative or path.endswith('/' + relative)


def _eval_budget_state(db, root_id, errors, messages):
    row = None
    if _eval_has_table(db, 'agent_budget_state'):
        row = db.execute('SELECT * FROM agent_budget_state WHERE root_session_id=?',
                         (root_id,)).fetchone()
    reminders = [message for message in messages if message['kind'] == 'BUDGET_REMINDER']
    # Limits and weights are runtime configuration, not persisted in this ledger.
    # Historical reminder text cannot establish the policy currently in force.
    return {'available': row is not None,
            'used_weighted_units': row['used'] if row else None,
            'input_tokens': row['input_tokens'] if row else None,
            'cached_tokens': row['cached_tokens'] if row else None,
            'output_tokens': row['output_tokens'] if row else None,
            'reminder_level': row['reminder_level'] if row else None,
            'limit': None, 'input_weight': None, 'output_weight': None,
            'reminder_at_remaining_tokens': None, 'policy_source': 'not_persisted',
            'classification': 'global_hard_stop_observed' if errors[-272] else
                              'request_budget_refusal_observed' if errors[-274] else
                              'child_allocation_refusal_observed' if errors[-273] else
                              'accounted_policy_unknown' if row else 'unknown',
            'global_hard_stop_observed': bool(errors[-272]),
            'child_allocation_refusals': errors[-273],
            'request_budget_refusals': errors[-274],
            'reminder_messages': len(reminders),
            'consumed_reminder_messages': sum(bool(message['consumed']) for message in reminders)}


def read_session(database, root_id=None):
    connection = sqlite3.connect(f'file:{Path(database).resolve()}?mode=ro', uri=True)
    connection.row_factory = sqlite3.Row
    with contextlib.closing(connection) as db:
        db.execute('BEGIN')
        if root_id is None:
            row = db.execute('SELECT id FROM sessions WHERE id NOT IN '
                             '(SELECT session_id FROM agent_threads) ORDER BY id DESC LIMIT 1').fetchone()
            if not row:
                raise ValueError('No primary session exists')
            root_id = row['id']
        root = db.execute('SELECT * FROM sessions WHERE id=?', (root_id,)).fetchone()
        if not root:
            raise ValueError('Primary session does not exist')
        agents = list(db.execute('SELECT * FROM agent_threads WHERE root_session_id=?', (root_id,)))
        session_ids = {root_id} | {row['session_id'] for row in agents}
        child_sessions = {row['session_id'] for row in agents}
        agent_sessions = {row['agent_id']: row['session_id'] for row in agents}
        turn_owners = collections.defaultdict(set)
        for row in db.execute("SELECT DISTINCT session_id,turn_id FROM messages "
                              "WHERE role='user' AND turn_id IS NOT NULL AND turn_id!=''"):
            turn_owners[row['turn_id']].add(row['session_id'])
        snapshots = {}
        if _eval_has_table(db, 'model_request_snapshots'):
            for row in db.execute('SELECT session_id,turn_id FROM model_request_snapshots'):
                if row['turn_id']:
                    turn_owners[row['turn_id']].add(row['session_id'])
                    snapshots[row['session_id']] = row['turn_id']
        attribution = collections.Counter()
        history = []
        for row in db.execute('SELECT id,session_id,sequence_no,turn_id,kind,content,payload_json,'
                              'tool_name,tool_call_id,provider_call_id,created_at '
                              'FROM model_history_items ORDER BY id'):
            if row['session_id'] not in session_ids:
                continue
            owners = turn_owners.get(row['turn_id'], set())
            if len(owners) != 1:
                attribution['unknown_records'] += 1
            elif row['session_id'] not in owners:
                attribution['excluded_inherited_records'] += 1
            else:
                attribution['owned_records'] += 1
                history.append(row)
        calls = collections.Counter()
        errors = collections.Counter()
        budget_errors = collections.Counter()
        reads = collections.Counter()
        assistant_responses = 0
        nonzero_processes = 0
        verification_records = 0
        primary_verifications = []
        primary_patch_records = []
        child_patch_records = []
        child_responses = collections.Counter()
        tool_calls = {}
        process_commands = {}
        unknown_receipts = 0
        times = []
        for row in history:
            session_id = row['session_id']
            times.append(row['created_at'])
            payload = _eval_json_object(row['payload_json'])
            outcome = _eval_json_object(payload.get('turn_outcome'))
            if outcome.get('error_code') in (-272, -273, -274):
                budget_errors[outcome['error_code']] += 1
            if row['kind'] in ('assistant_tool_calls', 'assistant_message') and session_id in child_sessions:
                if row['kind'] == 'assistant_tool_calls' or (
                        row['content'] and snapshots.get(session_id) == row['turn_id']):
                    child_responses[session_id] += 1
            if row['kind'] == 'assistant_tool_calls':
                assistant_responses += 1
                for call in payload.get('calls', []):
                    if not isinstance(call, dict):
                        continue
                    name = call.get('name', '')
                    calls[name] += 1
                    for identifier in (call.get('tool_call_id'), call.get('provider_call_id')):
                        if identifier:
                            tool_calls[(session_id, identifier)] = (row, call)
                    if name in ('file_read', 'file_list', 'file_info'):
                        arguments = call.get('arguments', call.get('input', ''))
                        reads[(session_id, name, str(arguments))] += 1
            elif row['kind'] == 'tool_result':
                metadata, data, successful = _eval_tool_receipt(row)
                error_code = metadata.get('error_code')
                if isinstance(error_code, int) and error_code < 0:
                    errors[row['tool_name'] or 'unknown'] += 1
                    if error_code in (-272, -273, -274):
                        budget_errors[error_code] += 1
                elif metadata.get('status') != 'completed':
                    unknown_receipts += 1
                call_row, call = tool_calls.get((session_id, row['tool_call_id']),
                    tool_calls.get((session_id, row['provider_call_id']), (None, {})))
                if row['tool_name'] == 'apply_patch' and successful and data.get('changed') is True:
                    files = [item.get('path') for item in data.get('files', [])
                             if isinstance(item, dict) and
                             (item.get('added', 0) > 0 or item.get('removed', 0) > 0)]
                    record = {'receipt_id': row['id'], 'session_id': session_id,
                              'call_id': call_row['id'] if call_row else None,
                              'header': any(_eval_task_path(path, 'include/task.h') for path in files),
                              'contract_prototypes_added': _eval_patch_contract(call),
                              'implementation': any(_eval_task_path(path, 'src/' + name + '.c')
                                                    for path in files for name in ('math', 'text'))}
                    (primary_patch_records if session_id == root_id else child_patch_records).append(record)
                if row['tool_name'] in ('exec', 'process') and successful:
                    process_id = data.get('session_id')
                    arguments = _eval_json_object(call.get('arguments'))
                    command = data.get('command') or arguments.get('command') or arguments.get('cmd')
                    if command and process_id:
                        process_commands[(session_id, process_id)] = command
                    command = command or process_commands.get((session_id, process_id), '')
                    if data.get('status') == 'exited':
                        if data.get('exit_code') not in (0, None):
                            nonzero_processes += 1
                        stdout = data.get('stdout', '')
                        verified = (data.get('exit_code') == 0 and isinstance(stdout, str) and
                                    ('ACCEPTANCE_OK' in stdout or '100% tests passed' in stdout) and
                                    isinstance(command, str) and
                                    ('ctest' in command or 'verify.c' in command or
                                     re.search(r'\./(?:verify|acceptance)\b', command)))
                        if verified:
                            verification_records += 1
                            if session_id == root_id:
                                primary_verifications.append(row['id'])
        messages = list(db.execute('SELECT sender,recipient,kind,consumed FROM agent_mailbox '
                                   'WHERE root_session_id=?', (root_id,)))
        results = [row for row in messages if row['kind'] == 'FINAL_ANSWER']
        intervals = []
        for row in agents:
            if row['ended_at'] > row['started_at'] > 0:
                intervals.extend(((row['started_at'], 1), (row['ended_at'], -1)))
        active = peak = 0
        for _, delta in sorted(intervals, key=lambda item: (item[0], item[1])):
            active += delta
            peak = max(peak, active)
        event_count = db.execute('SELECT COUNT(*) FROM agent_events WHERE root_session_id=?',
                                (root_id,)).fetchone()[0]
        ids = [str(root['display_id']), str(root_id)]
        credit = list(db.execute('SELECT input_tokens,output_tokens,metadata_json FROM credit_events '
                                 'WHERE session_id IN (?,?)', ids))
        usage = {'input_tokens': 0, 'output_tokens': 0, 'cached_tokens': 0,
                 'request_records': len(credit), 'provider_records': 0, 'estimated_records': 0}
        for row in credit:
            usage['input_tokens'] += row['input_tokens']
            usage['output_tokens'] += row['output_tokens']
            metadata = _eval_json_object(row['metadata_json'])
            usage['cached_tokens'] += min(max(0, metadata.get('cached_tokens', 0)),
                                          max(0, row['input_tokens']))
            source = metadata.get('usage_source')
            if source in ('provider', 'estimated'):
                usage[source + '_records'] += 1
        usage['uncached_input_tokens'] = usage['input_tokens'] - usage['cached_tokens']
        usage['unknown_source_records'] = (usage['request_records'] - usage['provider_records'] -
                                           usage['estimated_records'])
        usage['request_record_basis'] = 'positive_usage_credit_records_not_all_attempts'
        imported = (_eval_has_table(db, 'sync_import_receipts') and db.execute(
                    'SELECT 1 FROM sync_import_receipts WHERE session_id=? LIMIT 1',
                    (root_id,)).fetchone() is not None)
        dependency_patches = [record for record in child_patch_records if record['implementation']]
        contracts = [record['receipt_id'] for record in primary_patch_records
                     if record['header'] and record['contract_prototypes_added']]
        dependency_order = None
        if (not imported and not attribution['unknown_records'] and dependency_patches and
                all(record['call_id'] for record in dependency_patches)):
            if contracts:
                dependency_order = all(any(contract < record['call_id'] for contract in contracts)
                                       for record in dependency_patches)
            elif not any(record['header'] for record in primary_patch_records):
                dependency_order = False
        child_patched = {record['session_id'] for record in child_patch_records}
        productive_children = {record['session_id'] for record in dependency_patches} & child_responses.keys()
        consumed_children = {agent_sessions[message['sender']] for message in results
                             if message['consumed'] and message['sender'] in agent_sessions}
        integrated = None
        if not imported and not attribution['unknown_records']:
            integrated = bool(productive_children) and productive_children <= consumed_children and any(
                verification > max(record['receipt_id'] for record in child_patch_records)
                for verification in primary_verifications) if child_patch_records else False
        collaboration = {'task_correctness_observed': None,
                         'task_correctness_basis': 'requires_independent_acceptance',
                         'child_model_response_sessions': len(child_responses),
                         'child_model_response_records': sum(child_responses.values()),
                         'child_model_request_snapshot_sessions': sum(
                             session_id in snapshots for session_id in child_sessions),
                         'child_successful_patch_sessions': len(child_patched),
                         'child_successful_patch_records': len(child_patch_records),
                         'productive_child_sessions': len(productive_children),
                         'primary_successful_verification_records': len(primary_verifications),
                         'primary_contract_patch_records': len(contracts),
                         'child_implementation_patch_records': len(dependency_patches),
                         'delegation_observed': bool(child_responses) if not attribution['unknown_records'] else None,
                         'integration_observed': integrated,
                         'dependency_contract_before_child_implementation': dependency_order,
                         'dependency_evidence_basis': 'unknown_import_order' if imported else
                                                      'header_prototype_receipt_before_child_implementation_call',
                         'ordering_basis': 'model_history_items.id_persistence_order_not_timestamps',
                         'semantic_integration': 'not_proven_by_delivery_acknowledgment'}
        return {'session_id': root_id, 'display_id': root['display_id'], 'model': root['model'],
                'history_span_seconds': max(times) - min(times) if times else 0,
                'agents_created': len(agents), 'agents_completed': sum(row['status'] == 2 for row in agents),
                'agents_failed': sum(row['status'] == 3 for row in agents),
                'peak_child_executions': peak, 'peak_child_executions_basis': 'last_interval_per_agent',
                'tool_calls': dict(calls),
                'tool_failures': dict(errors),
                'delegation_failures': sum(errors[name] for name in ('spawn_agent', 'followup_task')),
                'identical_read_repeats': sum(count - 1 for count in reads.values()),
                'nonzero_processes': nonzero_processes, 'verification_records': verification_records,
                'result_messages': len(results), 'unconsumed_results': sum(not row['consumed'] for row in results),
                'agent_event_records': event_count, 'assistant_tool_rounds': assistant_responses,
                'history_attribution': {key: attribution[key] for key in
                    ('owned_records', 'excluded_inherited_records', 'unknown_records')},
                'unknown_tool_receipts': unknown_receipts,
                'collaboration': collaboration,
                'budget': _eval_budget_state(db, root_id, budget_errors, messages), 'usage': usage}


def patch(path, old, new):
    return ('*** Begin Patch\n*** Update File: ' + path + '\n@@\n' +
            ''.join('-' + line + '\n' for line in old.splitlines()) +
            ''.join('+' + line + '\n' for line in new.splitlines()) + '*** End Patch')


def tool_call(name, arguments, identifier):
    encoded = arguments if isinstance(arguments, str) else json.dumps(arguments)
    return {'index': 0, 'id': identifier, 'type': 'function',
            'function': {'name': name, 'arguments': encoded}}


def old_source(name):
    if name == 'math':
        return '#include "task.h"\nint clamp_add(int a, int b) { (void)a; (void)b; return 0; }\n'
    if name == 'text':
        return '#include "task.h"\nsize_t ascii_upper_count(const char *p, size_t n) { (void)p; (void)n; return 0; }\n'
    return '#include "task.h"\nint combined_score(int a, int b, const char *p, size_t n) { (void)a; (void)b; (void)p; (void)n; return 0; }\n'


def model_delta(body, state):
    messages = body.get('messages', [])
    systems = '\n'.join(message.get('content', '') for message in messages if message.get('role') == 'system')
    if "Summarize the user's underlying task" in systems:
        return {'content': '{"title":"Multi-agent evaluation"}'}
    proactive = 'Proactive multi-agent delegation is active' in systems
    if proactive != (state['mode'] == 'ultra'):
        return {'content': 'POLICY_MISMATCH: requested collaboration mode was not injected.'}
    actor = re.search(r'You are `([^`]+)`', systems)
    actor = actor.group(1) if actor else '/root'
    previous = []
    for message in messages:
        for call in message.get('tool_calls', []):
            previous.append(call.get('function', {}).get('name', call.get('name', '')))
    child = actor != '/root'
    if child:
        name = actor.rsplit('/', 1)[1]
        if 'apply_patch' not in previous:
            time.sleep(.1)
            return {'tool_calls': [tool_call('apply_patch', patch(f'src/{name}.c', old_source(name), SOURCES[name]), 'patch_' + name)]}
        return {'content': name + ' implementation complete; primary must run acceptance tests.'}
    calls = []
    if state['dependency'] and 'apply_patch' not in previous:
        old = '#ifndef EVAL_TASK_H\n#define EVAL_TASK_H\n#include <stddef.h>\nint combined_score(int a, int b, const char *data, size_t length);\n#endif\n'
        return {'tool_calls': [tool_call('apply_patch', patch('include/task.h', old, HEADER), 'define_contract')]}
    if state['mode'] == 'ultra':
        if 'spawn_agent' not in previous:
            calls.append(tool_call('update_plan', {'plan': [
                {'step': 'Implement independent modules', 'status': 'in_progress'},
                {'step': 'Integrate and verify', 'status': 'pending'}]}, 'plan_start'))
            for name in ('math', 'text'):
                calls.append(tool_call('spawn_agent', {'task_name': name,
                    'message': 'Implement only src/' + name + '.c as specified by include/task.h and TASK.txt. Return evidence. Do not change other files.',
                    'fork_turns': 'none', 'write_paths': [f'src/{name}.c']}, 'spawn_' + name))
            return {'tool_calls': calls}
        evidence = '\n'.join(str(message.get('content', '')) for message in messages)
        if 'math implementation complete' not in evidence or 'text implementation complete' not in evidence:
            return {'tool_calls': [tool_call('wait_agent', {'timeout_ms': 1000}, 'wait_workers_' + str(len(messages)))]}
    else:
        patched = sum(name == 'apply_patch' for name in previous) - int(state['dependency'])
        if patched < 2:
            name = ('math', 'text')[patched]
            return {'tool_calls': [tool_call('apply_patch', patch(f'src/{name}.c', old_source(name), SOURCES[name]), 'patch_' + name)]}
    report_patches = sum(name == 'apply_patch' for name in previous)
    expected_patches = (1 if state['mode'] == 'ultra' else 3) + int(state['dependency'])
    if report_patches < expected_patches:
        return {'tool_calls': [tool_call('apply_patch', patch('src/report.c', old_source('report'), REPORT), 'integrate_report')]}
    if 'exec' not in previous:
        return {'tool_calls': [tool_call('exec', {'command': VERIFY_COMMAND, 'yield_time_ms': 1000}, 'verify_task')]}
    receipt = next((message for message in reversed(messages) if message.get('role') == 'tool'), {})
    result = _eval_json_object(receipt.get('content'))
    data = result.get('data', result)
    if isinstance(data, dict) and data.get('status') == 'running' and data.get('session_id'):
        return {'tool_calls': [tool_call('process', {
            'action': 'wait', 'session_id': data['session_id'], 'wait_ms': 10000,
        }, 'collect_verify_' + str(len(messages)))]}
    return {'content': 'Integrated math and text implementations. ACCEPTANCE_OK: 12 assertions.'}


class ScriptedModel(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        delta = model_delta(body, self.server.eval_state)
        for index, call in enumerate(delta.get('tool_calls', [])):
            call['index'] = index
        payload = ('data: ' + json.dumps({'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]}) + '\n\n' +
                   'data: ' + json.dumps({'choices': [{'index': 0, 'delta': {}, 'finish_reason': 'stop'}],
                   'usage': {'prompt_tokens': 120, 'completion_tokens': 30, 'total_tokens': 150}}) +
                   '\n\ndata: [DONE]\n\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def task_prompt(dependency, require_delegation=False):
    return ('Implement the C11 APIs described in TASK.txt, including edge cases. '
            'You may modify src/math.c, src/text.c, src/report.c' +
            (' and include/task.h; establish the interface contract before dependent implementation.' if dependency else '.') +
            ' Preserve verify.c and run its acceptance tests with -Werror. '
            'Report test evidence and integration status. Avoid unrelated changes.' +
            (' Use spawn_agent to assign src/math.c and src/text.c to two separate agents '
             'with disjoint write_paths. Keep the shared header and src/report.c with the '
             'primary agent. Wait for both results and run the integrated acceptance tests '
             'yourself. This run explicitly tests delegated execution.' if require_delegation else ''))


def create_task(directory, dependency):
    (directory / 'include').mkdir()
    (directory / 'src').mkdir()
    header = HEADER if not dependency else ('#ifndef EVAL_TASK_H\n#define EVAL_TASK_H\n#include <stddef.h>\n'
                                           'int combined_score(int a, int b, const char *data, size_t length);\n#endif\n')
    (directory / 'include/task.h').write_text(header)
    for name in SOURCES:
        (directory / f'src/{name}.c').write_text(old_source(name))
    (directory / 'verify.c').write_text(VERIFY)
    (directory / 'TASK.txt').write_text(
        'clamp_add(int,int) returns the sum saturated at INT_MIN/INT_MAX without signed overflow.\n'
        'ascii_upper_count(const char*,size_t) counts only ASCII A-Z over the exact byte length, '
        'including embedded NUL; NULL returns zero.\n'
        'combined_score(int,int,const char*,size_t) applies clamp_add(a,b) then saturating-adds '
        'the uppercase count (cap count at INT_MAX before converting to int).\n'
        'The primary owns report.c and the shared API contract. Module workers, if used, own '
        'separate math.c and text.c files. No external dependencies or network use are needed.\n')
    (directory / 'AGENTS.md').write_text(
        '# Evaluation workspace\nUse only this workspace. Preserve verify.c and TASK.txt. '
        'Compile with C11 and -Werror; tests are the acceptance evidence. '
        'Use apply_patch for edits. Do not access external services or alter configuration.\n')


def toml_value(value):
    if isinstance(value, bool):
        return str(value).lower()
    if isinstance(value, (int, float)):
        return str(value)
    return json.dumps(value, ensure_ascii=False)


def make_config(path, model, mode, directory, token_budget):
    effective_model = dict(context_limit=32768, max_tokens=4096,
                           timeout_seconds=90, retry_count=0)
    allowed = ('provider', 'adapter', 'model', 'api_base', 'api_key_env', 'extra_body_json',
               'context_limit', 'max_tokens', 'timeout_seconds', 'retry_count')
    effective_model.update({key: model[key] for key in allowed if key in model})
    lines = ['[general]', f'output_dir={toml_value(str(directory / "output"))}',
             f'log_file={toml_value(str(directory / "agent.log"))}', 'auto_recap=false',
             '[features]', 'hooks=false', '[memory]', 'enabled=false',
             'learn_from_chats_enabled=false', 'save_tasks_enabled=false',
             '[permissions]', 'access="workspace"', 'approval="never"',
             '[react]', 'max_iterations=16', 'tool_max_retries=1',
             'disabled_tools=["config_edit","tool_create","tool_promote","tool_delete",'
             '"tasks","request_permissions","memory","memory_preference"]',
             '[agents]', 'enabled=true', 'max_threads=3', 'max_depth=2',
             'multi_agent_mode=' + toml_value('proactive' if mode == 'ultra' else 'explicit'),
             '[features.rollout_budget]', f'enabled={toml_value(token_budget > 0)}',
             'reminder_at_remaining_tokens=' + toml_value(
                 sorted({value for value in (token_budget * 3 // 10, token_budget // 10)
                         if 0 < value < token_budget}, reverse=True))]
    if token_budget:
        lines.append(f'limit_tokens={token_budget}')
    lines.append('[model.text]')
    lines.extend(key + '=' + toml_value(effective_model[key])
                 for key in allowed if key in effective_model)
    path.write_text('\n'.join(lines) + '\n')
    path.chmod(0o600)
    return {key: effective_model[key] for key in
            ('provider', 'adapter', 'model', 'context_limit', 'max_tokens',
             'timeout_seconds', 'retry_count') if key in effective_model}


def assess_trial(metrics, mode, dependency, execution_ok, acceptance):
    task_passed = bool(execution_ok and acceptance.get('tests_passed') and
                       acceptance.get('integrity_passed') and
                       metrics.get('unconsumed_results') == 0)
    collaboration = metrics.get('collaboration', {})
    delegated = bool(collaboration.get('delegation_observed'))
    integrated = bool(collaboration.get('integration_observed'))
    dependency_observed = collaboration.get('dependency_contract_before_child_implementation')
    orchestration_passed = bool(task_passed and delegated and integrated and
                                (not dependency or dependency_observed is True))
    status = 'passed' if orchestration_passed else 'not_observed' if not delegated else 'incomplete'
    return {'task_passed': task_passed, 'delegation_observed': delegated,
            'integration_observed': integrated, 'orchestration_status': status,
            'orchestration_passed': orchestration_passed,
            'passed': orchestration_passed if mode == 'ultra' else task_passed}


def stop_group(process, sig):
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process.pid, sig)


def run_trial(driver, runner, model, mode, dependency, output_dir, scripted, timeout,
              token_budget, require_delegation=False):
    name = ('dependency_contract' if dependency else 'independent_modules') + '-' + mode
    directory = output_dir / name
    directory.mkdir()
    create_task(directory, dependency)
    env = dict(os.environ, HOME=str(directory / 'home'), MORPH_DEBUG='0', NO_COLOR='1')
    Path(env['HOME']).mkdir()
    config = directory / 'config.toml'
    server = None
    if scripted:
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ScriptedModel)
        server.daemon_threads = True
        server.eval_state = {'mode': mode, 'dependency': dependency}
        threading.Thread(target=server.serve_forever, daemon=True).start()
        model = {'provider': 'openai', 'model': 'scripted-task-model', 'api_key_env': 'MORPH_EVAL_API_KEY',
                 'api_base': f'http://127.0.0.1:{server.server_port}/v1'}
        env['MORPH_EVAL_API_KEY'] = 'local-test'
    effective_model = make_config(config, model, mode, directory, token_budget)
    baseline = capture_integrity(directory, dependency=dependency)
    prompt = task_prompt(dependency, require_delegation=require_delegation)
    command = [str(driver), '-c', str(config), '-w', str(directory), '--no-color', '-p', prompt]
    started = time.monotonic()
    timed_out = False
    execution_error = None
    stdout = stderr = ''
    process_exit = None
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=env, text=True, start_new_session=True)
        stdout, stderr = process.communicate(timeout=timeout)
        process_exit = process.returncode
    except subprocess.TimeoutExpired:
        timed_out = True
        stop_group(process, signal.SIGTERM)
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            stop_group(process, signal.SIGKILL)
            stdout, stderr = process.communicate()
        process_exit = process.returncode
    except (OSError, ValueError) as error:
        execution_error = type(error).__name__
    finally:
        elapsed = time.monotonic() - started
        if server:
            server.shutdown()
            server.server_close()
    # Local raw logs are review artifacts; summaries deliberately contain no replies.
    for key_name in ('MORPH_EVAL_API_KEY', model.get('api_key_env', '')):
        secret = env.get(key_name)
        if secret:
            stdout = stdout.replace(secret, '[REDACTED]')
            stderr = stderr.replace(secret, '[REDACTED]')
    (directory / 'stdout.txt').write_text(stdout)
    (directory / 'stderr.txt').write_text(stderr)
    acceptance = verify_task(directory, runner, baseline, dependency=dependency)
    database = Path(env['HOME']) / '.morph/data.db'
    try:
        metrics = read_session(database) if database.exists() else {}
    except (sqlite3.Error, ValueError):
        metrics = {'analysis_unavailable': True}
    metrics.setdefault('collaboration', {}).update(
        task_correctness_observed=bool(acceptance.get('tests_passed') and
                                      acceptance.get('integrity_passed')),
        task_correctness_basis='trusted_external_verifier')
    budget = metrics.setdefault('budget', {})
    budget.update(limit=token_budget, input_weight=1, output_weight=1,
                  reminder_at_remaining_tokens=[token_budget * 3 // 10, token_budget // 10],
                  enforcement='hard' if token_budget else 'disabled',
                  policy_source='trial_config')
    used = budget.get('used_weighted_units') or 0
    budget['classification'] = ('unknown' if not budget.get('available') else
                                'disabled' if not token_budget else
                                'global_limit_reached' if used >= token_budget else
                                'warning' if used >= token_budget * .7 else 'within_allocation')
    result = {'task': 'dependency_contract' if dependency else 'independent_modules',
              'mode': mode, 'backend': 'scripted' if scripted else 'live',
              'delegation_request': 'explicit' if require_delegation else 'mode_policy_only',
              'elapsed_seconds': round(elapsed, 3), 'timed_out': timed_out,
              'process_exit': process_exit, 'execution_error': execution_error,
              'effective_model': effective_model, 'acceptance': acceptance, 'metrics': metrics}
    result.update(assess_trial(metrics, mode, dependency,
                              process_exit == 0 and not timed_out and not execution_error,
                              acceptance))
    print(json.dumps({'task': result['task'], 'mode': mode, 'passed': result['passed'],
                      'elapsed_seconds': result['elapsed_seconds'],
                      'agents': metrics.get('agents_created'), 'failures': metrics.get('tool_failures')},
                     ensure_ascii=False), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', type=Path)
    parser.add_argument('--runner', type=Path,
                        help='Sandboxed acceptance runner; defaults beside --driver')
    parser.add_argument('--provider-config', type=Path, help='Run live inference using this model.text profile')
    parser.add_argument('--session-db', type=Path, help='Analyze a real saved session without changing it')
    parser.add_argument('--session-id', type=int)
    parser.add_argument('--mode', choices=('normal', 'ultra', 'both'), default='both')
    parser.add_argument('--task', choices=('independent', 'dependency', 'both'), default='both')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--token-budget', type=int, default=120000,
                        help='Weighted session-tree units per trial (0 disables the limit)')
    parser.add_argument('--require-delegation', action='store_true',
                        help='Explicit delegation probe; do not compare with policy-only samples')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.session_db:
        report = {'schema_version': 2, 'backend': 'session-replay',
                  'metrics': read_session(args.session_db, args.session_id)}
    else:
        if not args.driver:
            parser.error('--driver is required for task runs')
        runner = (args.runner or args.driver.resolve().with_name('morph-agent-eval-runner')).resolve()
        if not runner.is_file():
            parser.error('Build morph-agent-eval-runner before task runs')
        if args.token_budget < 0 or args.timeout <= 0:
            parser.error('--token-budget must be nonnegative and --timeout must be positive')
        model = {}
        if args.provider_config:
            model = tomllib.loads(args.provider_config.read_text())['model']['text']
            key = model.get('api_key') or os.environ.get(model.get('api_key_env', ''))
            if not key:
                parser.error('Configured API key is unavailable in this process; run from the configured shell. Do not paste keys into chat.')
            os.environ['MORPH_EVAL_API_KEY'] = key
            model = dict(model, api_key_env='MORPH_EVAL_API_KEY')
        trial_dir = Path(tempfile.mkdtemp(prefix='morph-agent-eval-', dir=args.output.parent))
        modes = ('normal', 'ultra') if args.mode == 'both' else (args.mode,)
        dependencies = (False, True) if args.task == 'both' else (args.task == 'dependency',)
        trials = [run_trial(args.driver.resolve(), runner, model, mode, dependency, trial_dir,
                            not args.provider_config, args.timeout, args.token_budget,
                            args.require_delegation)
                  for dependency in dependencies for mode in modes]
        report = {'schema_version': 2, 'backend': 'live' if args.provider_config else 'scripted',
                  'artifact_directory': str(trial_dir), 'trials': trials,
                  'all_passed': all(trial['passed'] for trial in trials),
                  'driver_sha256': hashlib.sha256(args.driver.read_bytes()).hexdigest(),
                  'evaluator_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  'acceptance_runner_sha256': hashlib.sha256(runner.read_bytes()).hexdigest(),
                  'interpretation': 'Scripted runs validate orchestration and real C acceptance, not model planning quality. '
                                    'Live single samples describe outcomes, not a statistically established speedup. '
                                    'Identical read repeats may be legitimate after edits; delivery acknowledgment is not semantic integration.'}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print('Report: ' + str(args.output), flush=True)
    return 0 if report.get('all_passed', True) else 1


if __name__ == '__main__':
    raise SystemExit(main())
