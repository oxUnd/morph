---
name: code-review
description: Review code for bugs, style issues, and best practices. Use when the user asks for code review or feedback on code.
---

# Code Review Skill

When reviewing code, follow this systematic approach:

1. **Read the code** — Use file_read to load the file(s) mentioned by the user
2. **Analyze** — Check for:
   - Logic errors and potential bugs
   - Security vulnerabilities
   - Performance issues
   - Code style and naming conventions
   - Missing error handling
3. **Report** — Structure your review as:
   - Summary of what the code does
   - Issues found (ordered by severity: critical → warning → suggestion)
   - Specific line references where possible
   - Suggested fixes with code examples

Always be constructive. Explain *why* something is an issue, not just *that* it is one.
