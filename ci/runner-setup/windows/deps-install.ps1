# Windows self-hosted runner — build-dependency setup (fresh install).
#
# Run ONCE, in an ELEVATED PowerShell (Administrator), on the runner machine — e.g. BeefWin (the AMD/D3D12
# runner for amd-d3d12.yml). The GitHub Actions runner SERVICE is assumed already registered and Idle
# (Settings > Actions > Runners); this installs only the build toolchain the CI jobs need. It does not
# touch the runner service or its labels.
#
# Installs, all via winget (ships with Windows 10 21H2+/11 as "App Installer"):
#   - Git                         (actions/checkout + the SameBoy submodule)
#   - CMake 3.28+                 (the build system)
#   - Ninja                       (generator; VS generator is used by the jobs, Ninja is a convenience)
#   - Visual Studio 2022 Build Tools with:
#       * the C++ build tools workload (MSVC v143)
#       * the ClangCL toolset        (the jobs configure with -G "Visual Studio 17 2022" -T ClangCL)
#       * the Windows 11 SDK         (provides dxc.exe for the DXIL shader compile in gen_shader.cmake)
#
# SDL3 and GoogleTest are fetched from source by CMake FetchContent at configure time — no separate
# install. No AMD-specific SDK is needed: the bug repro is a normal D3D12 app on an AMD GPU + driver,
# which the OS/driver already provide.

$ErrorActionPreference = "Stop"

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install 'App Installer' from the Microsoft Store (or update Windows), then re-run."
}

Write-Host "== Git =="
winget install --id Git.Git --exact --silent `
    --accept-source-agreements --accept-package-agreements

Write-Host "== CMake =="
winget install --id Kitware.CMake --exact --silent `
    --accept-source-agreements --accept-package-agreements

Write-Host "== Ninja =="
winget install --id Ninja-build.Ninja --exact --silent `
    --accept-source-agreements --accept-package-agreements

Write-Host "== Visual Studio 2022 Build Tools (C++ + ClangCL + Windows 11 SDK) =="
# --override passes the VS installer its own component switches. --includeRecommended pulls the matching
# MSVC + CRT. Windows11SDK.22621 provides dxc.exe; bump the number if a newer SDK is preferred.
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent `
    --accept-source-agreements --accept-package-agreements `
    --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"

Write-Host ""
Write-Host "Done. Open a NEW terminal so PATH refreshes, then verify:"
Write-Host "    cmake --version"
Write-Host "    git --version"
Write-Host "    where dxc          # from the Windows SDK; used for the DXIL shader compile"
Write-Host ""
Write-Host "The runner can then build amd-d3d12.yml (it configures with the VS 2022 generator + ClangCL"
Write-Host "toolset and builds entirely inside \$RUNNER_TEMP — never C:\ root)."
