# Cursor workflow & principles

Follow these when working on this repo.

## Workflow orchestration

1. **Plan node default**  
   Enter plan mode for ANY non-trivial task (3+ steps or architectural decisions).  
   If something goes sideways, STOP and re-plan — don’t keep pushing.  
   Use plan mode for verification steps, not just building.  
   Write detailed specs upfront to reduce ambiguity.

2. **Subagent strategy**  
   Use subagents to keep the main context clean.  
   Offload research, exploration, and parallel analysis to subagents.  
   For complex problems, use more compute via subagents.  
   One tack per subagent for focused execution.

3. **Self-improvement loop**  
   After ANY correction from the user: update `CURSOR/lessons.md` with the pattern.  
   Write rules that prevent the same mistake.  
   Ruthlessly iterate on these lessons until mistake rate drops.  
   Review lessons at session start for the relevant project.

4. **Verification before done**  
   Never mark a task complete without proving it works.  
   Diff behavior between main and your changes when relevant.  
   Ask: “Would a staff engineer approve this?”  
   Run tests, check logs, demonstrate correctness.

5. **Demand elegance (balanced)**  
   For non-trivial changes: pause and ask “is there a more elegant way?”  
   If a fix feels hacky: “Knowing everything I know now, implement the elegant solution.”  
   Skip this for simple, obvious fixes — don’t over-engineer.  
   Challenge your own work before presenting it.

6. **Autonomous bug fixing**  
   When given a bug report: just fix it. Don’t ask for hand-holding.  
   Point at logs, errors, failing tests — then resolve them.  
   Zero context switching required from the user.  
   Go fix failing CI tests without being told how.

## Task management

- **Plan first:** Write plan to `CURSOR/todo.md` with checkable items.
- **Verify plan:** Check in before starting implementation.
- **Track progress:** Mark items complete as you go.
- **Explain changes:** High-level summary at each step.
- **Document results:** Add review section to `CURSOR/todo.md`.
- **Capture lessons:** Update `CURSOR/lessons.md` after corrections.

## Core principles

- **Simplicity first:** Make every change as simple as possible. Impact minimal code.
- **No laziness:** Find root causes. No temporary fixes. Senior developer standards.
- **Minimal impact:** Changes should only touch what’s necessary. Avoid introducing bugs.
