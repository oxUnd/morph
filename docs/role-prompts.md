# Role prompt design and sources

The setup presets are working instructions, not just role labels. Each rendered
prompt combines `prompts/common/scenario.txt` with the selected file under
`prompts/scenarios/`. The developer preset additionally includes the existing
`prompts/senior-engineer.txt`, keeping one authoritative engineering workflow.
No runtime or model-specific template syntax is introduced.

The common instructions establish task scope, permission boundaries, capability
honesty, evidence and delivery. Specialist instructions add decisions and checks
that matter in that domain. They are conditional workflows: a request for a quick
critique should not produce a complete production brief, and a request for a
finished artifact should not stop at a plan. The workflow headings describe
behavior, not a mandatory response template.

## Role behavior

| Preset | Specialized workflow and quality criteria |
| --- | --- |
| General assistant | Explanation, comparison, planning and practical action; use decision criteria, check calculations and verify resulting state. |
| Software developer | Reuse the full staff engineer instructions: inspect ownership and callers, preserve changes, debug from evidence, implement proportionately, verify and review the diff. |
| Photographer | Brief and coverage, light and composition, conditional exposure and focus advice, evidence-based critique, series selection and practical shoot handoff. |
| Photo editor | Inspect source and preservation constraints, choose deterministic or generative methods, retain originals, adapt batches by lighting, inspect artifacts and export appropriately. |
| Writer | Distinguish ideation, outlining, drafting, critique and copyediting; preserve voice, support nonfiction claims, maintain fictional continuity, revise at the requested level. |
| Designer | Translate the brief into hierarchy, type and composition; choose an appropriate production medium, distinguish interface and graphic needs, inspect actual outputs and provide a usable handoff. |
| Video creator | Brief, script and timed shot plan, capability-aware asset production, continuity, editing and sound, playable export inspection; distinguish clips from an assembled film. |
| Researcher | Decompose the question, target valuable uncertainties, evaluate sources, compare quantities, challenge hypotheses, synthesize and attach evidence to claims. |

Setup's required model categories remain separate from instructions and tool
permissions. In particular, the video preset requires text, vision, image and
video models. That does not require every task to invoke all four, and it does
not assert the presence of an editor, audio generator or arbitrary video decoder.
Role text does not grant permissions or silently install skills.

## Research and adaptation

Reviewed on **2026-10-30**. These are public original sources, including actual
agent prompts and specialist workflow references. They inform the instructions;
they do not establish a universal best prompt or measured performance in Morph.

Published agent prompt revisions are pinned for reproducibility:

- Anthropic `claude-cookbooks`:
  `c43f56d0360b9b1f631bd090fd199231d4c880cc`.
- Agency Agents: `b37b5bb33b78f2c1d6567d98e25ca818e3259219`.

| Original source | What Morph uses | What was deliberately adapted |
| --- | --- | --- |
| [Anthropic research lead prompt](https://github.com/anthropics/claude-cookbooks/blob/c43f56d0360b9b1f631bd090fd199231d4c880cc/patterns/agents/prompts/research_lead_agent.md) | Task decomposition, choosing research depth or breadth, source evaluation, synthesis and stopping when the question is adequately answered. | Morph may research directly; delegation is conditional. Removed fixed tool names, mandatory delegation, instructions to expose detailed reasoning, and the external citation-agent assumption. Morph supplies its own citations. |
| [Agency Agents: UI Designer](https://github.com/msitarzewski/agency-agents/blob/b37b5bb33b78f2c1d6567d98e25ca818e3259219/design/design-ui-designer.md) | Visual hierarchy, consistency, accessibility considerations and a usable design handoff. | Adapted to graphic as well as interface design. A screenshot does not establish accessibility compliance; a small asset does not require a new design system. |
| [Agency Agents: Visual Storyteller](https://github.com/msitarzewski/agency-agents/blob/b37b5bb33b78f2c1d6567d98e25ca818e3259219/design/design-visual-storyteller.md) | Audience-led narrative, storyboards, pacing and consistency across assets. | No invented memory or professional track record, assumed project paths, or fixed engagement targets. |
| [Agency Agents: Content Creator](https://github.com/msitarzewski/agency-agents/blob/b37b5bb33b78f2c1d6567d98e25ca818e3259219/marketing/marketing-content-creator.md) | Audience, voice, medium-specific adaptation and the distinction between planning and producing content. | Writer also supports fiction, nonfiction and editing. Removed marketing-only assumptions and unsupported growth or conversion promises. |
| [Anthropic: Building effective agents](https://www.anthropic.com/research/building-effective-agents) | Proportionate workflows, tool feedback and evaluation before claiming improvement. | This historical engineering reference is not a requirement to adopt a framework or a particular agent architecture. |
| [Nikon: A Basic Look at the Basics of Exposure](https://www.nikonusa.com/learn-and-explore/c/tips-and-techniques/a-basic-look-at-the-basics-of-exposure) | Explain the aperture, shutter and ISO tradeoff in the photographer's shooting conditions. | Numerical settings remain starting points, not universal prescriptions or inferred capture metadata. |
| [Adobe: Non-destructive editing techniques](https://helpx.adobe.com/photoshop/desktop/create-manage-layers/create-layer-compositions/non-destructive-editing-techniques.html) | Preserve source data and use reversible adjustments, masks and retouch layers where supported. | Do not assume Photoshop is installed or claim a layered master when the tool produced only a raster image. |
| [Purdue OWL: Reverse Outlining](https://owl.purdue.edu/owl/general_writing/the_writing_process/reverse_outlining.html) | Inspect each section's contribution to the argument when structural revision is requested. | An editing technique, not a copied worksheet or a required ritual for short copyedits. |
| [Blackmagic Design: DaVinci Resolve training](https://www.blackmagicdesign.com/products/davinciresolve/training) | Separate rough cut, refinement, color, audio and delivery checks. | No assumption that Resolve or audio tools are available; asset generation is distinct from assembly. |

Specialist manuals are references for professional techniques, not text included
in the prompt. The adapted agent workflows are written for Morph's capabilities
and are not verbatim copies of the external prompts. MIT notices for the two
agent prompt collections ship in `prompts/NOTICE` alongside the templates.

## Verification and behavioral review

The existing setup tests render every role using the real template library,
check that bindings resolve, validate model requirements and exercise snapshot
replacement while preserving custom files. The template tests cover includes,
escaping, invalid syntax and bounds. These checks establish that the prompts can
be generated and saved; they do not establish model behavior or writing quality.

Use the following cases for model evaluation before making performance claims.
Keep the same model/version, tool fixtures, generation settings and brief when
comparing old and new prompts. Include simple requests as well as full tasks,
and record actual artifacts and tool evidence. Assess completion, accuracy,
scope, preservation, domain quality and unnecessary tool/token cost separately.

| Role | Representative cases | Observable review criteria |
| --- | --- | --- |
| Common | Discuss a change without implementing it; request execution with a missing tool. | No unauthorized edits; completes available work and states the specific gap without fabricated success. |
| General | Explain a small concept; compare options against a supplied budget. | Direct explanation without ceremony; recommendation follows the user's criteria and checked arithmetic. |
| Developer | Diagnose a failing case in a dirty worktree; review without edit authorization. | Evidence identifies the cause; unrelated changes survive; relevant checks run; review findings have a trigger and consequence. |
| Photographer | Plan an indoor moving-subject shoot with existing equipment; critique a low-resolution image without EXIF. | Settings include conditions and tradeoffs; advice is feasible; critique separates observation from an unverified cause. |
| Photo editor | Match a reference across mixed-light portraits; export an exact crop of a product photo. | Batch treatment adapts to source groups, originals survive, identity and geometry remain faithful; the final output is inspected. |
| Writer | Copyedit without changing voice; produce fiction; revise a factual article with an unsupported statistic. | Edit scope and voice are preserved, fiction remains coherent, unsupported facts are verified or identified rather than invented. |
| Designer | Produce a small poster with exact copy; review a screenshot for accessibility concerns. | The artifact contains correct copy and usable dimensions; review does not claim a completed accessibility audit. |
| Video creator | Plan a timed short; request a final video with only a silent clip generator available. | Timing and shot continuity are coherent; returned clips are distinguished from a finished edit; no invented sound or render. |
| Researcher | Compare conflicting, differently dated reports; answer a narrow lookup with retrieval unavailable. | Dates, definitions and source dependence are checked; claims are traceable; access limits are explicit and scope stays proportionate. |

Human review should also check whether extra instructions make a simple task
needlessly elaborate. No live-model comparative evaluation is implied by passing
the renderer or setup tests.

## Loading and customization

Setup renders a plain-text snapshot and references it from `[prompt]`. Upgrading
the templates does not hot-reload a running session or automatically replace a
user's existing snapshot. Re-run `morph setup`, select role instructions and save
to generate the current version, then start Morph again. Modified snapshot files
are preserved, and setup creates a separate snapshot as needed. See
[setup](setup.md) for saving and backup details.
