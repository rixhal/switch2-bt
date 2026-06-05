## Checklist

Tick all that apply before requesting review.

### Code quality
- [ ] `python -m py_compile switch2d.py` passes
- [ ] `python -m pytest tests/ -v` passes
- [ ] No new warnings introduced

### Documentation
- [ ] Docs updated if behavior changed (`docs/` files)
- [ ] README status banner accurate

### Architecture boundaries
- [ ] No raw-HCI, BTstack, HCI_CHANNEL_USER, kernel-code, or protocol-logic changes
- [ ] If this PR intentionally modifies raw-HCI/BTstack/kernel code, explain why below

### Hardware validation
- [ ] If this PR claims Linux wireless is working, a hardware golden run has been
      completed and the following are attached or linked:
  - Golden-run diagnostic JSON
  - `golden_run.jsonl` (reports)
  - btmon capture (optional but recommended)

**Do not claim success without hardware logs.**
