# Scheduled Tasks and Notifications

## Problem

Some user requests cannot finish inside one ReAct turn. They create work that
must happen later, possibly more than once, and may finish only when an external
condition changes.

Examples:

- Remind me at 15:00 to pick up coffee.
- Check every two minutes whether my coffee is ready.
- Summarize yesterday's meetings every weekday morning.
- Check a build until it passes or times out.

These are not just calendar events. A calendar answers "when is something due".
Agent automation also needs task state, an executable action, retry policy,
completion conditions, and notification delivery.

## User Model

Expose this feature as Tasks. This matches how users describe the work:

- a task has a title and status;
- it may have a due time or repeat rule;
- it may be manual, a reminder, or an executable automation;
- it completes, fails, times out, or can be cancelled.

Avoid exposing "calendar" as the main abstraction. A calendar view can be added
later as a presentation layer over scheduled tasks.

## Internal Model

Internally, represent executable tasks as scheduled jobs.

Core concepts:

- Task: the durable user-visible item.
- Trigger: when a task should run.
- Action: what happens when the task runs.
- Policy: how retries, timeouts, and stop conditions work.
- Notification: where results are delivered.

Task kinds:

- `reminder`: notify the user at a scheduled time.
- `action`: run one tool or agent step at a scheduled time.
- `watch`: run an action repeatedly until a stop condition is met.

Initial statuses:

- `pending`: created, waiting for first run.
- `running`: currently being executed.
- `waiting`: executed but waiting for the next scheduled run.
- `completed`: finished successfully.
- `failed`: finished with an unrecoverable error.
- `cancelled`: stopped by user or policy.
- `timed_out`: stopped because the timeout policy was reached.

## Notification Targets

Notifications must not depend on the original CLI session still being alive.

Initial targets:

- `inbox`: write a notification into Morph's SQLite store.
- `session`: best-effort push to the active session if still connected.

Future targets:

- `desktop`: system notification.
- `chat`: Feishu, Slack, Telegram, or similar.
- `email`: email delivery.
- `webhook`: user-configured HTTP endpoint.

The default target should be `inbox`. Session delivery is an enhancement, not a
source of truth.

## MVP Scope

The first implementation should provide the durable foundation without trying to
solve every automation path at once.

MVP includes:

- SQLite tables for scheduled tasks and notifications.
- CRUD helpers for creating, updating, listing, and cancelling tasks.
- Notification insertion and read/unread state.
- A scheduler-facing query for due tasks.
- A simple runner contract that can later be called by CLI, FastCGI, or a daemon.

MVP does not include:

- Desktop, IM, email, or webhook delivery.
- Natural-language date parsing beyond what the current agent/tool layer can
  provide.
- A long-running daemon requirement.
- Complex condition expression evaluation.
- Calendar UI.

## Storage Shape

Suggested tables:

```text
scheduled_tasks
- id
- title
- kind
- status
- trigger_type
- next_run_at
- interval_seconds
- timeout_at
- attempts
- max_attempts
- action_type
- payload_json
- policy_json
- notify_json
- last_error
- created_at
- updated_at

notifications
- id
- task_id
- level
- title
- body
- created_at
- read_at
- delivery_status
```

Time values should be Unix seconds in UTC.

## Execution Flow

Reminder:

```text
create task
  -> scheduler finds task when next_run_at <= now
  -> insert inbox notification
  -> mark completed
```

Watch task:

```text
create task
  -> scheduler finds task when next_run_at <= now
  -> run action
  -> if stop condition met: notify and complete
  -> else if timeout or max attempts reached: notify and timed_out/failed
  -> else compute next_run_at and mark waiting
```

## Open Questions

- Should scheduled jobs be driven by the CLI opportunistically, a FastCGI
  process, or a dedicated daemon?
- Should watch conditions be fixed per action type at first, or represented as a
  small JSON expression language?
- How should agent context be restored for `agent_run` actions?
- Should tasks be global, session-scoped, or both?
- What is the CLI surface: `morph tasks`, a tool callable by the agent, or both?

## Recommended First Cut

Start with a storage and API layer. Then add a CLI command or tool that creates a
manual reminder/watch task using explicit parameters. After the persistence
model is stable, wire the ReAct loop to create tasks from user intent.
