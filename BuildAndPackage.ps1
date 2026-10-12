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

.PARAMETER SkipDeploy
    Skip copying the plugin and shaders into the MO2 mod folder

.PARAMETER DeployDir
    MO2 mod folder to deploy into (e.g. ...\MODDING MO2\mods\SkyrimRenderer_main). Defaults to
    $env:SKYRIMRENDERER_DEPLOY_DIR, then to $DeployDir set in BuildAndPackage.local.ps1 (gitignored)
    next to this script. With none set, the deploy step is skipped.

.EXAMPLE
    .\BuildAndPackage.ps1
    .\BuildAndPackage.ps1 -SkipZip
#>

param(
    [switch]$SkipCpp,
    [switch]$SkipZip,
    [switch]$SkipDeploy,
    [string]$DeployDir
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
            # `call` is required: without it cmd hands control to vcvarsall.bat and never runs $Cmd.
            $batContent = "@call `"" + $vcvars + "`" x64 > nul 2>&1`r`n" +
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

# -- [1/3] C++ Build ----------------------------------------------------------

if (-not $SkipCpp) {
    Write-Header "[1/3] C++ Release Build"

    $buildDir = Join-Path $ProjectDir "build\release-msvc"
    if (-not (Test-Path $buildDir)) {
        Invoke-MsvcCmd -Label "CMake configure (build-release-msvc)" `
            -Cmd "cmake --preset build-release-msvc"
    }

    Invoke-MsvcCmd -Label "C++ Release build" `
        -Cmd "cmake --build build/release-msvc --config Release"
} else {
    Write-BuildWarn "[1/3] C++ build skipped"
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

# -- [2/3] Zip ----------------------------------------------------------------

if (-not $SkipZip) {
    Write-Header "[2/3] Packaging Distribution"

    $distDir = Join-Path $ProjectDir "contrib\Distribution"
    $outDir  = Join-Path $ProjectDir "contrib"
    $branch  = git -C $ProjectDir rev-parse --abbrev-ref HEAD 2>$null
    if (-not $branch) { $branch = "unknown" }
    $zipName = ("SkyrimRenderer_" + $branch + ".zip") -replace '[/\\]', '-'
    $zipPath = Join-Path $outDir $zipName

    if (-not (Test-Path $distDir)) {
        Write-Err "Distribution folder not found: $distDir"
        exit 1
    }

    Write-Info "Output: $zipPath"
    Compress-Archive -Path "$distDir\*" -DestinationPath $zipPath -CompressionLevel Optimal -Force
    Write-Ok "Archive created: $zipPath"
} else {
    Write-BuildWarn "[2/3] Zip skipped"
}

# -- [3/3] Deploy to MO2 -------------------------------------------------------

if (-not $SkipDeploy) {
    Write-Header "[3/3] Deploying to MO2"

    if (-not $DeployDir) { $DeployDir = $env:SKYRIMRENDERER_DEPLOY_DIR }
    $localCfg = Join-Path $ProjectDir "BuildAndPackage.local.ps1"
    if (-not $DeployDir -and (Test-Path $localCfg)) { . $localCfg }

    if (-not $DeployDir) {
        Write-BuildWarn "No deploy folder (pass -DeployDir, set SKYRIMRENDERER_DEPLOY_DIR or BuildAndPackage.local.ps1) - skipped"
    } elseif (Get-Process -Name "SkyrimSE" -ErrorAction SilentlyContinue) {
        Write-Err "SkyrimSE.exe is running (the DLL is loaded) - close the game and deploy again"
        exit 1
    } else {
        $dll        = Join-Path $ProjectDir "contrib\Distribution\PluginRelease\SkyrimRenderer.dll"
        $pluginsDir = Join-Path $DeployDir "SKSE\Plugins"
        $shadersDst = Join-Path $pluginsDir "SkyrimRenderer\shaders"
        if (-not (Test-Path $dll)) {
            Write-Err "Plugin not built: $dll"
            exit 1
        }
        New-Item -ItemType Directory -Force $pluginsDir | Out-Null
        Copy-Item $dll $pluginsDir -Force
        if (Test-Path $shadersDst) { Remove-Item $shadersDst -Recurse -Force }
        New-Item -ItemType Directory -Force $shadersDst | Out-Null
        Copy-Item (Join-Path $ProjectDir "contrib\Distribution\Shaders\*") $shadersDst -Recurse -Force
        $ver = (Get-Item (Join-Path $pluginsDir "SkyrimRenderer.dll")).VersionInfo.FileVersion
        Write-Ok "Deployed SkyrimRenderer $ver to $DeployDir"
    }
} else {
    Write-BuildWarn "[3/3] Deploy skipped"
}

# -- Done ---------------------------------------------------------------------

Write-Host ""
Write-Host "===============================================" -ForegroundColor Green
Write-Host "  SkyrimRenderer Build and Package Complete!"   -ForegroundColor Green
Write-Host "===============================================" -ForegroundColor Green
Write-Host ""
