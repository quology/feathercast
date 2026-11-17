# Automatic task delegation

Before beginning each new task, analyze its scope, complexity, risk, and required
expertise. Select an agent automatically and delegate implementation to it. Do
not ask the user to select a model. Use GPT-6.1 Sol as the main coordinator when
available; its default is set in `.codex/config.toml`.

| Agent | Model / reasoning | Route these tasks here |
| --- | --- | --- |
| `architect` | `gpt-6-astra` / `high` | Architecture, low-level C++, libobs integration, dynamic delay, complex bugs, performance optimization. |
| `developer` | `gpt-6.1-sol` / `medium` | Feature implementation, backend development, integration, debugging, tests. Escalate difficult work to higher reasoning or architect. |
| `ui` | `gpt-6.1-terra` / `medium` | Qt UI, layout, styling, simple interface changes. |
| `simple` | `gpt-6-luna` / `low` | Documentation, straightforward fixes, formatting, repetitive maintenance. |

Choose the least expensive suitable agent. For mixed tasks, divide work into
bounded steps and route each step by its actual complexity. Architectural,
low-level, libobs, dynamic-delay, or complex performance risks take priority over
surface-level task labels.

## Sequential execution

1. Use only one active subagent at a time. Never launch parallel agents or allow
   subagents to delegate further. The coordinator owns all delegation.
2. Give the selected agent the objective, acceptance criteria, relevant paths,
   constraints, decisions, completed work, and validation expectations. Spawn the
   registered custom agent by name when the client supports this capability.
3. Wait for completion, inspect the result, and finish appropriate validation
   before delegating again. Do not edit the assigned files concurrently.
4. Reuse capacity only after completion and validation. The installed CLI's
   current agent runtime was verified to accept a new named agent after the
   previous one completed with the limit set to one; it exposes no close tool.
   On clients that retain completed threads against the limit, close or release
   them through the supported lifecycle mechanism. If no slot can be released,
   report the limitation instead of bypassing the limit or starting parallel work.
5. Preserve context between agents: include changed files, decisions, validation
   commands and results, unresolved failures, and the next concrete action in each
   handoff. Inspect current files; do not reset or discard user edits or prior
   agents' completed work.

## Escalation and cost control

Escalate difficult or repeated failures to `architect` with reproduction steps,
evidence, attempted fixes, and remaining hypotheses. Validate the architect's
result, then return to `developer`, `ui`, or `simple` as soon as the remaining work
is straightforward. Do not keep using the most expensive model unnecessarily.

## Client limitations and verification

Honor higher-priority instructions and actual tool capabilities. If named custom
agent spawning is unavailable but explicit model overrides are supported, use a
fresh agent with the selected model and reasoning effort and supply that custom
agent's `developer_instructions` plus a complete handoff. This is an explicit
override fallback, not proof that named-agent registration was exercised.

If model selection or the selected model is unavailable, explain the limitation
and use an available suitable model automatically when it can safely complete
the task. Do not silently claim the configured model ran. Confirm execution
identity from runtime metadata when exposed; configuration, a spawn request, and
an agent's self-report alone do not prove which model executed. Report any
unsupported model/reasoning combination or unavailable execution metadata.

## FeatherCast development

Target Windows 11 x64, C++20, Qt 6, and standalone libobs. The project is licensed
GPL-3.0-or-later; preserve third-party notices and corresponding-source obligations.
Never put stream keys in JSON exports, logs, screenshots, or Git. Keep optional
modules and the UI separate from encoding and packet transport.

Only the coordinator manages Git commits, pushes, tags, or releases. The user has
authorized automatic synchronization of validated changes to the private repository
`quology/feathercast`. Never force-push or change visibility. Exclude `work/`,
`outputs/`, build caches, and secrets from commits. Record actual build/test results,
known gaps, and verified model usage. Never label incomplete milestones stable or
claim performance improvements without equivalent measured workloads.
