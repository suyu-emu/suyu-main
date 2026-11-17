# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding(DefaultParameterSetName = "BuildHost")]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputRoot,

    [Parameter(Mandatory = $true)]
    [string]$WorkRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$RecompilerPath,

    [Parameter(Mandatory = $true, ParameterSetName = "BuildHost")]
    [string]$HostBuildRoot,

    [Parameter(Mandatory = $true, ParameterSetName = "ProvidedHost")]
    [string]$StaticHostPath,

    [Parameter(ParameterSetName = "BuildHost")]
    [string]$CMakePath = "cmake",

    [Parameter(ParameterSetName = "BuildHost")]
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",

    [Parameter(ParameterSetName = "BuildHost")]
    [ValidateRange(1, 128)]
    [int32]$BuildJobs = 4,

    [ValidateSet("Application", "Applet")]
    [string]$LaunchMode = "Application",

    [uint32]$AppletId = 1,

    [ValidateSet("Application", "LibraryApplet", "OverlayApplet", "SystemApplet")]
    [string]$AppletType = "Application",

    [ValidateSet("FrontendInitiated", "ApplicationInitiated")]
    [string]$LaunchType = "FrontendInitiated",

    [ValidateRange(0, 2147483647)]
    [int32]$ProgramIndex = 0,

    [int32]$PreviousProgramIndex = -1,

    [switch]$AllowUnignoredOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($env:OS -ne "Windows_NT") {
    throw "package_deconstructed.ps1 currently supports Windows hosts only"
}

$InvariantCulture = [System.Globalization.CultureInfo]::InvariantCulture
$ModuleOrder = @(
    "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
    "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"
)

function Resolve-ExistingPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet("File", "Directory")][string]$Kind
    )

    $item = Get-Item -LiteralPath $Path -Force
    if ($Kind -eq "File" -and $item.PSIsContainer) {
        throw "Expected a file, but found a directory: $Path"
    }
    if ($Kind -eq "Directory" -and -not $item.PSIsContainer) {
        throw "Expected a directory, but found a file: $Path"
    }
    return $item.FullName
}

function Get-NewAbsolutePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

function Test-IsSameOrDescendant {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    $candidatePath = $Candidate.TrimEnd([char[]]"\/")
    $parentPath = $Parent.TrimEnd([char[]]"\/")
    if ([string]::Equals($candidatePath, $parentPath,
                        [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return $candidatePath.StartsWith(
        $parentPath + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-NewPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (Test-Path -LiteralPath $Path) {
        throw "$Name must be a new path and must not already exist: $Path"
    }
}

function Assert-IgnoredWhenInsideRepository {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$RepositoryRoot
    )

    if ($AllowUnignoredOutput -or
        -not (Test-IsSameOrDescendant -Candidate $Path -Parent $RepositoryRoot)) {
        return
    }

    $gitCommands = @(Get-Command git -CommandType Application -ErrorAction SilentlyContinue)
    if ($gitCommands.Count -eq 0) {
        throw "$Name is inside the source tree, but git is unavailable to verify that it is ignored: $Path"
    }
    $git = $gitCommands[0]

    $relative = $Path.Substring($RepositoryRoot.Length).TrimStart([char[]]"\/")
    $relative = $relative.Replace('\', '/')
    & $git.Source -C $RepositoryRoot check-ignore --quiet --no-index -- $relative
    if ($LASTEXITCODE -ne 0) {
        throw "$Name is inside the source tree and is not ignored by git. Use a build/ path or pass -AllowUnignoredOutput explicitly: $Path"
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Contents
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Contents, $encoding)
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-RequiredJsonProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "inspect-nso JSON is missing required property '$Name'"
    }
    return $property.Value
}

function Normalize-Hex {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][int]$Length
    )

    $text = ([string]$Value).Trim()
    $pattern = "^[0-9A-Fa-f]{${Length}}$"
    if ($text -notmatch $pattern) {
        throw "$Name must contain exactly $Length hexadecimal characters"
    }
    return $text.ToLowerInvariant()
}

function Convert-JsonUnsignedInteger {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$BareSixteenDigitHex
    )

    $text = ([string]$Value).Trim()
    $style = [System.Globalization.NumberStyles]::None
    if ($text.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        $text = $text.Substring(2)
        $style = [System.Globalization.NumberStyles]::AllowHexSpecifier
    } elseif ($BareSixteenDigitHex -and $text -match '^[0-9A-Fa-f]{16}$') {
        $style = [System.Globalization.NumberStyles]::AllowHexSpecifier
    } elseif ($text -notmatch '^[0-9]+$') {
        throw "$Name is neither an unsigned decimal integer nor a 0x-prefixed integer"
    }

    [uint64]$parsed = 0
    if (-not [uint64]::TryParse($text, $style, $InvariantCulture, [ref]$parsed)) {
        throw "$Name is outside the UInt64 range"
    }
    return $parsed
}

function Get-CommandPath {
    param([Parameter(Mandatory = $true)][string]$Command)

    if (Test-Path -LiteralPath $Command -PathType Leaf) {
        return (Resolve-ExistingPath -Path $Command -Kind File)
    }
    $resolved = @(Get-Command $Command -CommandType Application -ErrorAction Stop)
    return $resolved[0].Source
}

function Invoke-CheckedNativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$Description = $Executable
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function New-RegistrationSource {
    param([Parameter(Mandatory = $true)][string[]]$Modules)

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.AppendLine("/* auto-generated by package_deconstructed.ps1 - DO NOT EDIT */")
    [void]$builder.AppendLine("#include <stdint.h>")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("typedef void (*SuyuRecompBlockFn)(void*);")
    foreach ($module in $Modules) {
        [void]$builder.AppendLine("extern SuyuRecompBlockFn recomp_image_lookup_${module}(uint64_t);")
        [void]$builder.AppendLine("extern void recomp_image_set_base_${module}(uint64_t);")
        [void]$builder.AppendLine("extern const uint8_t* recomp_image_build_id_${module}(void);")
        [void]$builder.AppendLine("extern const uint8_t* recomp_image_text_sha256_${module}(void);")
        [void]$builder.AppendLine("extern uint64_t recomp_image_text_size_${module}(void);")
    }
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("typedef struct {")
    [void]$builder.AppendLine("    const char* name;")
    [void]$builder.AppendLine("    SuyuRecompBlockFn (*lookup)(uint64_t);")
    [void]$builder.AppendLine("    void (*set_base)(uint64_t);")
    [void]$builder.AppendLine("    const uint8_t* (*build_id)(void);")
    [void]$builder.AppendLine("    const uint8_t* (*text_sha256)(void);")
    [void]$builder.AppendLine("    uint64_t (*text_size)(void);")
    [void]$builder.AppendLine("} SuyuRecompStaticModule;")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("const SuyuRecompStaticModule* suyu_recomp_static_modules_v2(unsigned* count);")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("static const SuyuRecompStaticModule s_modules[] = {")
    foreach ($module in $Modules) {
        [void]$builder.AppendLine("    {`"${module}`", recomp_image_lookup_${module}, recomp_image_set_base_${module},")
        [void]$builder.AppendLine("     recomp_image_build_id_${module}, recomp_image_text_sha256_${module},")
        [void]$builder.AppendLine("     recomp_image_text_size_${module}},")
    }
    [void]$builder.AppendLine("};")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("const SuyuRecompStaticModule* suyu_recomp_static_modules_v2(unsigned* count) {")
    [void]$builder.AppendLine("    *count = (unsigned)(sizeof(s_modules) / sizeof(s_modules[0]));")
    [void]$builder.AppendLine("    return s_modules;")
    [void]$builder.AppendLine("}")
    return $builder.ToString()
}

$repositoryRoot = Resolve-ExistingPath -Path (Join-Path $PSScriptRoot "../..") -Kind Directory
$inputPath = Resolve-ExistingPath -Path $InputRoot -Kind Directory
$workPath = Get-NewAbsolutePath -Path $WorkRoot
$outputPath = Get-NewAbsolutePath -Path $OutputRoot
$recompiler = Resolve-ExistingPath -Path $RecompilerPath -Kind File
$exefsPath = Resolve-ExistingPath -Path (Join-Path $inputPath "exefs") -Kind Directory
$mainPath = Resolve-ExistingPath -Path (Join-Path $exefsPath "main") -Kind File
$npdmPath = Resolve-ExistingPath -Path (Join-Path $exefsPath "main.npdm") -Kind File

Assert-NewPath -Path $workPath -Name "WorkRoot"
Assert-NewPath -Path $outputPath -Name "OutputRoot"
if ((Test-IsSameOrDescendant -Candidate $workPath -Parent $inputPath) -or
    (Test-IsSameOrDescendant -Candidate $outputPath -Parent $inputPath)) {
    throw "WorkRoot and OutputRoot must not be inside InputRoot"
}
if ((Test-IsSameOrDescendant -Candidate $workPath -Parent $outputPath) -or
    (Test-IsSameOrDescendant -Candidate $outputPath -Parent $workPath)) {
    throw "WorkRoot and OutputRoot must not contain one another"
}
Assert-IgnoredWhenInsideRepository -Path $workPath -Name "WorkRoot" -RepositoryRoot $repositoryRoot
Assert-IgnoredWhenInsideRepository -Path $outputPath -Name "OutputRoot" -RepositoryRoot $repositoryRoot

$siblingRomfsPath = Join-Path $inputPath "romfs"
$siblingRomfsFile = Join-Path $inputPath "romfs.bin"
$embeddedRomfsFile = Join-Path $exefsPath "romfs.bin"
$embeddedRomfsDirectory = Join-Path $exefsPath "romfs"
if (Test-Path -LiteralPath $embeddedRomfsDirectory -PathType Container) {
    throw "An extracted RomFS must be a sibling of exefs, not exefs/romfs"
}
if (Test-Path -LiteralPath $embeddedRomfsDirectory -PathType Leaf) {
    throw "A packed RomFS inside exefs must be named romfs.bin; a bare romfs file may be a sibling of exefs"
}
$romfsCandidates = @()
if (Test-Path -LiteralPath $siblingRomfsPath -PathType Container) {
    $romfsCandidates += "directory"
}
if (Test-Path -LiteralPath $siblingRomfsPath -PathType Leaf) {
    $romfsCandidates += "sibling-packed-bare"
}
if (Test-Path -LiteralPath $siblingRomfsFile -PathType Leaf) {
    $romfsCandidates += "sibling-packed"
}
if (Test-Path -LiteralPath $embeddedRomfsFile -PathType Leaf) {
    $romfsCandidates += "embedded-packed"
}
if ($romfsCandidates.Count -gt 1) {
    throw "InputRoot contains more than one RomFS representation; keep exactly one"
}

if ($LaunchMode -eq "Applet" -and $AppletId -eq 0) {
    throw "AppletId must be nonzero for LaunchMode Applet"
}

$appletTypeValues = @{
    Application = 0
    LibraryApplet = 1
    OverlayApplet = 2
    SystemApplet = 3
}
$launchTypeValues = @{
    FrontendInitiated = 0
    ApplicationInitiated = 1
}
if ($LaunchMode -eq "Application") {
    $manifestAppletId = 1
    $manifestAppletType = 0
    $manifestLaunchType = 0
    $manifestProgramIndex = 0
    $manifestPreviousProgramIndex = -1
} else {
    $manifestAppletId = [uint32]$AppletId
    $manifestAppletType = [int]$appletTypeValues[$AppletType]
    $manifestLaunchType = [int]$launchTypeValues[$LaunchType]
    $manifestProgramIndex = $ProgramIndex
    $manifestPreviousProgramIndex = $PreviousProgramIndex
}

New-Item -ItemType Directory -Path $workPath | Out-Null
$aotPath = Join-Path $workPath "aot"
New-Item -ItemType Directory -Path $aotPath | Out-Null

$moduleRecords = @()
$emittedModules = @()
$programIdValue = $null
$programIdHex = $null

foreach ($module in $ModuleOrder) {
    $modulePath = Join-Path $exefsPath $module
    if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
        continue
    }

    Write-Host "Inspecting NSO module $module..."
    $inspectionLines = @(& $recompiler inspect-nso --input $modulePath --npdm $npdmPath --json)
    if ($LASTEXITCODE -ne 0) {
        throw "inspect-nso failed for module '$module' with exit code $LASTEXITCODE"
    }
    $inspectionText = $inspectionLines -join [Environment]::NewLine
    try {
        $inspection = $inspectionText | ConvertFrom-Json
    } catch {
        throw "inspect-nso returned invalid JSON for module '$module': $($_.Exception.Message)"
    }

    if ((Get-RequiredJsonProperty -Object $inspection -Name "architecture") -ne "aarch64") {
        throw "Module '$module' is not a validated AArch64 NSO"
    }
    if ((Get-RequiredJsonProperty -Object $inspection -Name "segments_decoded") -ne $true) {
        throw "Module '$module' could not be decoded; compressed NSOs require an LZ4-enabled suyu_recomp build"
    }
    if ((Get-RequiredJsonProperty -Object $inspection -Name "required_hashes_verified") -ne $true) {
        throw "Module '$module' did not pass all required NSO segment hashes"
    }

    $moduleProgramId = Convert-JsonUnsignedInteger `
        -Value (Get-RequiredJsonProperty -Object $inspection -Name "program_id") `
        -Name "program_id" -BareSixteenDigitHex
    if ($null -eq $programIdValue) {
        $programIdValue = $moduleProgramId
        $programIdHex = $programIdValue.ToString("x16", $InvariantCulture)
    } elseif ($programIdValue -ne $moduleProgramId) {
        throw "Module '$module' reported a different program ID than the first module"
    }

    $buildId = Normalize-Hex `
        -Value (Get-RequiredJsonProperty -Object $inspection -Name "build_id") `
        -Name "build_id" -Length 64
    $mappedTextHash = Normalize-Hex `
        -Value (Get-RequiredJsonProperty -Object $inspection -Name "mapped_text_sha256") `
        -Name "mapped_text_sha256" -Length 64
    $mappedTextSize = Convert-JsonUnsignedInteger `
        -Value (Get-RequiredJsonProperty -Object $inspection -Name "mapped_text_size") `
        -Name "mapped_text_size"
    if ($mappedTextSize -eq 0 -or ($mappedTextSize % 0x1000) -ne 0) {
        throw "mapped_text_size for module '$module' must be a nonzero multiple of 0x1000"
    }

    $textSegments = @($inspection.segments | Where-Object { $_.name -eq "text" })
    if ($textSegments.Count -ne 1) {
        throw "inspect-nso did not report exactly one text segment for module '$module'"
    }
    $decodedTextSize = Convert-JsonUnsignedInteger `
        -Value (Get-RequiredJsonProperty -Object $textSegments[0] -Name "decoded_size") `
        -Name "segments[text].decoded_size"

    $moduleOutput = Join-Path $aotPath $module
    Write-Host "Emitting AOT project for $module..."
    Invoke-CheckedNativeCommand -Executable $recompiler -Description "emit-nso for '$module'" `
        -Arguments @(
            "emit-nso", "--input", $modulePath, "--npdm", $npdmPath, "--module", $module,
            "--title", "Local deconstructed title", "--output", $moduleOutput
        )

    $emittedModules += $module
    $moduleRecords += [pscustomobject][ordered]@{
        name = $module
        build_id = $buildId
        nso_sha256 = Get-Sha256 -Path $modulePath
        nso_size = [uint64](Get-Item -LiteralPath $modulePath).Length
        decoded_text_size = [uint64]$decodedTextSize
        mapped_text_size = [uint64]$mappedTextSize
        mapped_text_sha256 = $mappedTextHash
        required_hashes_verified = $true
    }
}

if ($emittedModules.Count -eq 0 -or -not ($emittedModules -contains "main")) {
    throw "InputRoot did not contain an emittable main NSO"
}
if ($LaunchMode -eq "Applet" -and $programIdValue -eq 0) {
    throw "A nonzero NPDM program ID is required for applet launch"
}

$registrationPath = Join-Path $aotPath "recomp_registration.c"
Write-Utf8NoBom -Path $registrationPath `
    -Contents (New-RegistrationSource -Modules $emittedModules)

$hostProvenance = "provided"
if ($PSCmdlet.ParameterSetName -eq "BuildHost") {
    $hostBuildPath = Resolve-ExistingPath -Path $HostBuildRoot -Kind Directory
    $cachePath = Join-Path $hostBuildPath "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "HostBuildRoot must already be a configured suyu build tree: $hostBuildPath"
    }
    if ((Test-IsSameOrDescendant -Candidate $workPath -Parent $hostBuildPath) -or
        (Test-IsSameOrDescendant -Candidate $outputPath -Parent $hostBuildPath)) {
        throw "WorkRoot and OutputRoot must not be inside HostBuildRoot"
    }

    $cmake = Get-CommandPath -Command $CMakePath
    Write-Host "Configuring the per-title static host..."
    Invoke-CheckedNativeCommand -Executable $cmake -Description "host CMake configure" `
        -Arguments @("-S", $repositoryRoot, "-B", $hostBuildPath,
                     "-DSUYU_CMD_RECOMP_DIR=$aotPath")
    Write-Host "Building suyu-cmd-static (the executable will not be run)..."
    Invoke-CheckedNativeCommand -Executable $cmake -Description "suyu-cmd-static build" `
        -Arguments @("--build", $hostBuildPath, "--target", "suyu-cmd-static",
                     "--config", $Configuration, "--parallel",
                     $BuildJobs.ToString($InvariantCulture))

    $hostCandidates = @(
        (Join-Path $hostBuildPath "bin/$Configuration/suyu-cmd-static.exe"),
        (Join-Path $hostBuildPath "bin/suyu-cmd-static.exe"),
        (Join-Path $hostBuildPath "$Configuration/suyu-cmd-static.exe"),
        (Join-Path $hostBuildPath "suyu-cmd-static.exe")
    )
    $hostExecutable = $null
    foreach ($candidate in $hostCandidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $hostExecutable = (Resolve-ExistingPath -Path $candidate -Kind File)
            break
        }
    }
    if ($null -eq $hostExecutable) {
        throw "The suyu-cmd-static build succeeded, but its executable could not be found"
    }
    $hostProvenance = "built"
} else {
    $hostExecutable = Resolve-ExistingPath -Path $StaticHostPath -Kind File
    Write-Warning "Using a provided static host. Its embedded module set cannot be proven until runtime identity checks run."
}

$outputParent = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    New-Item -ItemType Directory -Path $outputParent | Out-Null
}
$stagePath = "$outputPath.partial-$PID-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $stagePath | Out-Null

try {
    Copy-Item -LiteralPath $exefsPath -Destination $stagePath -Recurse
    $romfsRecord = $null
    if ($romfsCandidates -contains "directory") {
        Copy-Item -LiteralPath $siblingRomfsPath -Destination $stagePath -Recurse
        $romfsRecord = [pscustomobject][ordered]@{
            layout = "directory"
            path = "romfs"
        }
    } elseif ($romfsCandidates -contains "sibling-packed-bare") {
        Copy-Item -LiteralPath $siblingRomfsPath `
            -Destination (Join-Path $stagePath "exefs/romfs.bin")
        $romfsRecord = [pscustomobject][ordered]@{
            layout = "packed"
            path = "exefs/romfs.bin"
        }
    } elseif ($romfsCandidates -contains "sibling-packed") {
        Copy-Item -LiteralPath $siblingRomfsFile `
            -Destination (Join-Path $stagePath "exefs/romfs.bin")
        $romfsRecord = [pscustomobject][ordered]@{
            layout = "packed"
            path = "exefs/romfs.bin"
        }
    } elseif ($romfsCandidates -contains "embedded-packed") {
        $romfsRecord = [pscustomobject][ordered]@{
            layout = "packed"
            path = "exefs/romfs.bin"
        }
    }

    $packagedHostPath = Join-Path $stagePath "suyu-recompiled.exe"
    Copy-Item -LiteralPath $hostExecutable -Destination $packagedHostPath

    $programIdDecimal = $programIdValue.ToString($InvariantCulture)
    # Give RealVfs an absolute path. A relative top-level `exefs` directory has
    # no representable parent in RealVfsDirectory, which prevents the loader
    # from discovering a sibling extracted RomFS at the package root.
    $commandLine = '"suyu-recompiled.exe" --game "%~dp0exefs\main"'
    if ($LaunchMode -eq "Applet") {
        $appletParameters = "{0},{1},{2},{3},{4},{5}" -f `
            $programIdDecimal, $manifestAppletId, $manifestAppletType, $manifestLaunchType,
            $manifestProgramIndex, $manifestPreviousProgramIndex
        $commandLine += " --applet-params=`"$appletParameters`""
    }
    $commandLine += " %*"
    $launchContents = @(
        "@echo off",
        'pushd "%~dp0"',
        $commandLine,
        'set "suyu_exit=%errorlevel%"',
        "popd",
        "exit /b %suyu_exit%",
        ""
    ) -join "`r`n"
    Write-Utf8NoBom -Path (Join-Path $stagePath "launch.cmd") -Contents $launchContents

    $localOnlyContents = @(
        "LOCAL DEVELOPMENT PACKAGE",
        "",
        "This directory contains executable code and assets supplied locally by the user.",
        "Do not commit or redistribute it unless you have the legal right to do so.",
        "The packager accepts decrypted ExeFS/RomFS input only; it does not accept console keys.",
        ""
    ) -join "`r`n"
    Write-Utf8NoBom -Path (Join-Path $stagePath "LOCAL_ONLY.txt") `
        -Contents $localOnlyContents

    $sourceRevision = "unknown"
    $sourceDirty = $true
    $gitCommands = @(Get-Command git -CommandType Application -ErrorAction SilentlyContinue)
    if ($gitCommands.Count -ne 0) {
        $git = $gitCommands[0]
        $revisionLines = @(& $git.Source -C $repositoryRoot rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and $revisionLines.Count -eq 1 -and
            $revisionLines[0] -match '^[0-9A-Fa-f]{40}$') {
            $sourceRevision = $revisionLines[0].ToLowerInvariant()
            $statusLines = @(& $git.Source -C $repositoryRoot status --porcelain `
                                 --untracked-files=no 2>$null)
            if ($LASTEXITCODE -eq 0) {
                $sourceDirty = $statusLines.Count -ne 0
            }
        }
    }

    $npdmHash = Get-Sha256 -Path $npdmPath
    $hostHash = Get-Sha256 -Path $packagedHostPath
    $manifest = [pscustomobject][ordered]@{
        format = "suyu-deconstructed-recomp-package"
        format_version = 1
        local_only = $true
        static_module_abi = 2
        generator = [pscustomobject][ordered]@{
            source_revision = $sourceRevision
            source_dirty = [bool]$sourceDirty
            recompiler_sha256 = Get-Sha256 -Path $recompiler
            host_provenance = $hostProvenance
        }
        program = [pscustomobject][ordered]@{
            program_id = $programIdHex
            npdm_sha256 = $npdmHash
            exefs_path = "exefs"
            romfs = $romfsRecord
        }
        launch = [pscustomobject][ordered]@{
            mode = $LaunchMode.ToLowerInvariant()
            applet_id = [uint32]$manifestAppletId
            applet_type = [int]$manifestAppletType
            launch_type = [int]$manifestLaunchType
            program_index = [int]$manifestProgramIndex
            previous_program_index = [int]$manifestPreviousProgramIndex
        }
        modules = [object[]]$moduleRecords
        artifacts = [pscustomobject][ordered]@{
            executable = "suyu-recompiled.exe"
            executable_size = [uint64](Get-Item -LiteralPath $packagedHostPath).Length
            executable_sha256 = $hostHash
            checksums = "SHA256SUMS"
        }
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 8
    Write-Utf8NoBom -Path (Join-Path $stagePath "recomp_package.json") `
        -Contents ($manifestJson + "`n")

    $relativeFiles = New-Object System.Collections.Generic.List[string]
    foreach ($file in Get-ChildItem -LiteralPath $stagePath -Recurse -File) {
        $relative = $file.FullName.Substring($stagePath.Length).TrimStart([char[]]"\/")
        $relative = $relative.Replace('\', '/')
        if ($relative.Contains("`r") -or $relative.Contains("`n")) {
            throw "Package paths containing line breaks cannot be represented in SHA256SUMS"
        }
        [void]$relativeFiles.Add($relative)
    }
    $relativeFiles.Sort([System.StringComparer]::Ordinal)
    $checksumBuilder = New-Object System.Text.StringBuilder
    foreach ($relative in $relativeFiles) {
        $nativeRelative = $relative.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        $hash = Get-Sha256 -Path (Join-Path $stagePath $nativeRelative)
        [void]$checksumBuilder.AppendLine("$hash  $relative")
    }
    Write-Utf8NoBom -Path (Join-Path $stagePath "SHA256SUMS") `
        -Contents $checksumBuilder.ToString()

    Move-Item -LiteralPath $stagePath -Destination $outputPath
} catch {
    Write-Warning "Packaging stopped. Any partial staging directory was left at: $stagePath"
    throw
}

Write-Host "Local package created at: $outputPath"
Write-Host "Generated AOT workspace retained at: $workPath"
Write-Host "The packaged executable was not run."
