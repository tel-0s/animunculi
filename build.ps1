# Build, test and package Animunculi. Optionally deploy the mod folder into a mod manager.
#   .\build.ps1                          # build + native tests + Papyrus + MCP tests + package
#   .\build.ps1 -Deploy -DeployTo <dir>  # ...and copy dist\Animunculi there (e.g. MO2's mods\Animunculi)
# Machine-specific settings (ANIMUNCULI_CS_ROOT, ANIMUNCULI_DEPLOY_DIR, SKYRIM_ROOT, ...) go in
# build.local.ps1 next to this script (git-ignored); see build.local.example.ps1.
param(
    [switch]$Deploy,
    [string]$DeployTo,
    [switch]$SkipTests
)
# Native tools write progress to stderr; rely on exit codes, not PowerShell error records.
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
$local = Join-Path $PSScriptRoot "build.local.ps1"
if (Test-Path $local) { . $local }
if (-not $DeployTo) { $DeployTo = $env:ANIMUNCULI_DEPLOY_DIR }

cmake --preset vs2026
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
cmake --build --preset release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# Every CommonLib function we link must have both an SE and an AE address (see the script).
python scripts\audit_relocations.py
if ($LASTEXITCODE -ne 0) { throw "relocation audit failed" }

if (-not $SkipTests) {
    & .\build\vs2026\Release\animunculi_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "native tests failed" }
}

python scripts\compile_papyrus.py
if ($LASTEXITCODE -eq 3) { Write-Warning "Papyrus compile skipped; packaging the committed .pex" }
elseif ($LASTEXITCODE -ne 0) { throw "Papyrus compile failed" }

if (-not $SkipTests) {
    Push-Location mcp
    # isolated env: works even while an agent session is running the MCP from mcp\.venv
    uv run --no-project --with "mcp>=2.2,<3" --with pytest python -m pytest -q
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw "MCP tests failed" }

    # MCPL conformance against a real host: the Claude Code bridge's McplServerHandle
    $bridge = $env:MCPL_BRIDGE_DIR
    if ($bridge -and (Get-Command bun -ErrorAction SilentlyContinue) -and (Test-Path $bridge)) {
        Push-Location mcp
        uv run --no-project --with "mcp>=2.2,<3" -- bun run tests/conformance/cc_bridge_host.ts
        $code = $LASTEXITCODE
        Pop-Location
        if ($code -ne 0) { throw "MCPL bridge conformance failed" }
    } else {
        Write-Host "skipping MCPL bridge conformance (needs bun and a mcpl-cc-bridge checkout: set MCPL_BRIDGE_DIR to its plugins\mcpl-bridge)"
    }
}

python scripts\package.py
if ($LASTEXITCODE -ne 0) { throw "packaging failed" }

if ($Deploy) {
    if (-not $DeployTo) { throw "-Deploy needs -DeployTo <mods folder>\Animunculi (or ANIMUNCULI_DEPLOY_DIR)" }
    New-Item -ItemType Directory -Force $DeployTo | Out-Null
    Copy-Item -Recurse -Force "dist\Animunculi\*" "$DeployTo\"
    Write-Host "Deployed to $DeployTo (enable it in your mod manager)"
}
