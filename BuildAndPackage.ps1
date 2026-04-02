#Requires -Version 5.1
<#
.SYNOPSIS
    SkyrimRenderer Build and Package Script

.DESCRIPTION
    Builds the SkyrimRenderer SKSE plugin (Release) and zips the distribution.
    Automatically sets up the MSVC environment via vcvarsall.bat.

.PARAMETER SkipCpp
    Skip the C++ cmake build step

.PARAMETER SkipZip
    Skip the final zip packaging step

.EXAMPLE
    .\BuildAndPackage.ps1
    .\BuildAndPackage.ps1 -SkipZip
#>

param(
    [switch]$SkipCpp,
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot

# -- Logging ------------------------------------------------------------------

function Write-Header($msg) { Write-Host "" ; Write-Host $msg -ForegroundColor Cyan }
function Write-Ok($msg)     { Write-Host "[OK]    $msg" -ForegroundColor Green }
function Write-Info($msg)   { Write-Host "[INFO]  $msg" -ForegroundColor Cyan }
function Write-BuildWarn($msg) { Write-Host "[WARN]  $msg" -ForegroundColor Yellow }
function Write-Err($msg)    { Write-Host "[ERROR] $msg" -ForegroundColor Red }
function Write-Dim($msg)    { Write-Host "        $msg" -ForegroundColor DarkGray }

# -- MSVC Environment ---------------------------------------------------------

function Find-Vcvarsall {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }

    try {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($vsPath) {
            $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $vcvars) { return $vcvars }
        }
    } catch { }
    return $null
}

function Invoke-MsvcCmd {
    param(
        [string]$Label,
        [string]$Cmd,
        [string]$WorkDir = $ProjectDir,
        [bool]$Required = $true
    )

    $vcvars = Find-Vcvarsall
    $tempBat = [System.IO.Path]::GetTempFileName() + ".bat"
    try {
        if ($vcvars) {
            Write-Dim "vcvarsall: $vcvars"
            $batContent = "@`"" + $vcvars + "`" x64 > nul 2>&1`r`n" +
                          "@cd /d `"$WorkDir`"`r`n" +
                          $Cmd
        } else {
            Write-BuildWarn "vcvarsall.bat not found - build may fail"
            $batContent = "@cd /d `"$WorkDir`"`r`n" + $Cmd
        }
        [System.IO.File]::WriteAllText($tempBat, $batContent, [System.Text.Encoding]::ASCII)

        Write-Dim ("$ " + $Cmd)
        cmd /c $tempBat
        $code = $LASTEXITCODE
    } finally {
        Remove-Item $tempBat -ErrorAction SilentlyContinue
    }

    if ($code -ne 0) {
        if ($Required) {
            Write-Err "$Label failed (exit code $code)"
            exit 1
        } else {
            Write-BuildWarn "$Label failed (optional, continuing)"
            return $false
        }
    }
    Write-Ok "$Label succeeded"
    return $true
}

# -- Banner -------------------------------------------------------------------

Write-Host ""
Write-Host "===============================================" -ForegroundColor Magenta
Write-Host "  SkyrimRenderer Build and Package"             -ForegroundColor Magenta
Write-Host "===============================================" -ForegroundColor Magenta

# -- [1/2] C++ Build ----------------------------------------------------------

if (-not $SkipCpp) {
    Write-Header "[1/2] C++ Release Build"

    $buildDir = Join-Path $ProjectDir "build\release-msvc"
    if (-not (Test-Path $buildDir)) {
        Invoke-MsvcCmd -Label "CMake configure (build-release-msvc)" `
            -Cmd "cmake --preset build-release-msvc"
    }

    Invoke-MsvcCmd -Label "C++ Release build" `
        -Cmd "cmake --build build/release-msvc --config Release"
} else {
    Write-BuildWarn "[1/2] C++ build skipped"
}

# -- Copy shaders into distribution -------------------------------------------

Write-Header "Syncing shaders to distribution"

$shaderSrc = Join-Path $ProjectDir "shaders"
$shaderDst = Join-Path $ProjectDir "contrib\Distribution\Shaders"
if (Test-Path $shaderSrc) {
    if (Test-Path $shaderDst) { Remove-Item $shaderDst -Recurse -Force }
    Copy-Item $shaderSrc $shaderDst -Recurse
    Write-Ok "Copied shaders to distribution"
} else {
    Write-BuildWarn "Shaders directory not found: $shaderSrc"
}

# -- [2/2] Zip ----------------------------------------------------------------

if (-not $SkipZip) {
    Write-Header "[2/2] Packaging Distribution"

    $distDir = Join-Path $ProjectDir "contrib\Distribution"
    $outDir  = Join-Path $ProjectDir "contrib"
    $branch  = git -C $ProjectDir rev-parse --abbrev-ref HEAD 2>$null
    if (-not $branch) { $branch = "unknown" }
    $stamp   = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
    $zipName = ("SkyrimRenderer_" + $branch + "_" + $stamp + ".zip") -replace '[/\\]', '-'
    $zipPath = Join-Path $outDir $zipName

    if (-not (Test-Path $distDir)) {
        Write-Err "Distribution folder not found: $distDir"
        exit 1
    }

    Write-Info "Output: $zipPath"
    Compress-Archive -Path "$distDir\*" -DestinationPath $zipPath -CompressionLevel Optimal -Force
    Write-Ok "Archive created: $zipPath"
} else {
    Write-BuildWarn "[2/2] Zip skipped"
}

# -- Done ---------------------------------------------------------------------

Write-Host ""
Write-Host "===============================================" -ForegroundColor Green
Write-Host "  SkyrimRenderer Build and Package Complete!"   -ForegroundColor Green
Write-Host "===============================================" -ForegroundColor Green
Write-Host ""
