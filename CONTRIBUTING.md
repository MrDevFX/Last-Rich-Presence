# Contributing

## Workflow

1. Keep changes behavior-preserving unless the task explicitly calls for product changes.
2. Run local verification before handing work off:

```powershell
.\scripts\verify.ps1 -Configuration Debug -Platform x64
```

3. When touching startup, tray, packaging, or activation logic, include a manual smoke pass for:
   - first launch
   - hidden-to-tray startup
   - second-launch restore
   - settings persistence after restart

## Code Style

- C++ style is defined by `.editorconfig` and `.clang-format`.
- Prefer small helpers over growing `MainWindow` or detector files further.
- Keep generated files, build outputs, and packaging artifacts out of source control.
- Do not hand-edit `Generated Files`.

## Verification

- `scripts/verify.ps1` is the local source of truth for build, JavaScript checks, native tests, release-artifact validation, unpackaged launch smoke, and packaged `Release-MSIX` smoke cleanup.
- `.\scripts\verify.ps1 -Configuration Release-MSIX -Platform x64` is the packaged smoke path on a machine that can deploy and run the package, and it removes any smoke-added current-user certificate trust entries before exiting.
- The GitHub Actions `Debug` smoke lane must target a self-hosted Windows runner labeled `interactive-desktop`; the other matrix lanes stay on `-SkipLaunchSmoke`.

## Pull Requests

- Summarize user-visible behavior changes and verification performed.
- Call out any remaining manual smoke items or release-lane gaps explicitly.
