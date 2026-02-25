# Git hooks

These hooks run for this repo. **prepare-commit-msg** removes any `Co-authored-by:` lines from commit messages so commits are attributed only to you.

To use them, run once:

```bash
git config core.hooksPath .githooks
```

(Already done for this clone if you see commits without co-author trailers.)
