# SPDX-License-Identifier: Apache-2.0
#
# Pace is a NEW product, and the product it was renamed from -- fastcached -- is DETECTED by
# its upgrade code, never upgraded (packaging/windows/service-actions.xml). This proves it on
# a real install, against the real fastcached 0.5.0 MSI:
#
#   1. download fastcached 0.5.0's MSI and check it is that family;
#   2. install it (exit 0 or 3010);
#   3. install Pace over it: it must FAIL, its log must carry the launch condition's message,
#      and no Pace product may be left registered;
#   4. uninstall fastcached through its own path, then install Pace (the service table row
#      the default install leaves);
#   5. uninstall Pace.
#
# Every failure names its step. The old product's transactions run through plain msiexec:
# Invoke-Msiexec judges THIS table's services, which fastcached does not register under these
# names.
#
#   pwsh -NoProfile -File scripts/msi-refuses-retired-product.ps1 -Package <Pace.msi>
#
# Run from the directory the logs should land in (the MSI job's build tree).

param(
    [Parameter(Mandatory)] [string] $Package
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/MsiServiceTable.psm1')

$retiredRelease = 'v0.5.0'
$retiredUpgradeCode = '{581D183D-47A3-44FC-97E9-4B5491E792D8}'
# packaging/windows/service-actions.xml's Launch message, up to its pointer at the guide.
$refusal = 'fastcached is installed. Uninstall it (Settings > Apps > fastcached), then install Pace.'

function Invoke-RawMsiexec([string[]] $Arguments) {
    Write-Host "msiexec $($Arguments -join ' ')"
    return (Start-Process msiexec.exe -ArgumentList $Arguments -Wait -PassThru).ExitCode
}

$downloads = Join-Path $env:RUNNER_TEMP 'retired-product'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null
gh release download $retiredRelease --repo LASTRADA-Software/pace --pattern 'fastcached-0.5.0-*.msi' --dir $downloads --clobber
if ($LASTEXITCODE -ne 0) { throw "step 1: could not download the $retiredRelease MSI (gh exited $LASTEXITCODE)" }
$retired = (Get-ChildItem -Path $downloads -Filter *.msi | Select-Object -First 1).FullName
if (-not $retired) { throw "step 1: release $retiredRelease carries no MSI, so the refusal cannot be tested" }
if ((Get-MsiProperty $retired 'UpgradeCode') -ne $retiredUpgradeCode) {
    throw "step 1: $retired is not of the retired family $retiredUpgradeCode"
}
$pace = (Resolve-Path -LiteralPath $Package).Path
$family = Get-MsiProperty $pace 'UpgradeCode'
if ($family -eq $retiredUpgradeCode) { throw "step 1: Pace's MSI still carries the retired upgrade code $family" }
# The condition is the package's own row, so the refusal below is THIS condition rather than
# some other failure that happens to exit non-zero.
$message = Get-MsiLaunchConditionMessage $pace 'Installed OR NOT RETIRED_FASTCACHED_PRODUCT'
if (-not $message) { throw "step 1: Pace's MSI has no LaunchCondition refusing RETIRED_FASTCACHED_PRODUCT" }
if (-not $message.StartsWith($refusal)) { throw "step 1: Pace's LaunchCondition says '$message', not '$refusal...'" }

$code = Invoke-RawMsiexec @('/i', "`"$retired`"", '/qn', '/l*v', "`"$(Join-Path $PWD 'retired-install.log')`"")
if ($code -notin 0, 3010) { throw "step 2: installing $retiredRelease exited $code" }

$code = Invoke-RawMsiexec @('/i', "`"$pace`"", '/qn', '/l*v', "`"$(Join-Path $PWD 'refused.log')`"")
if ($code -eq 0) { throw "step 3: Pace installed over $retiredRelease (exit 0): the launch condition did not refuse it" }
if (-not (Select-String -LiteralPath 'refused.log' -SimpleMatch $refusal -Quiet)) {
    Show-MsiLog 'refused.log'
    throw "step 3: the install exited $code, but refused.log does not carry the condition's message, so something else failed"
}
$left = Get-InstalledProductCodes $family
if ($left.Count -ne 0) { throw "step 3: the refused install left a Pace product registered" }
Write-Host "step 3: refused with exit $code and the condition's message"

$code = Invoke-RawMsiexec @('/x', "`"$retired`"", '/qn', '/l*v', "`"$(Join-Path $PWD 'retired-uninstall.log')`"")
if ($code -notin 0, 3010) { throw "step 4: uninstalling $retiredRelease exited $code" }
$left = Get-InstalledProductCodes $retiredUpgradeCode
if ($left.Count -ne 0) { throw "step 4: $retiredRelease is still registered after its uninstall" }

Invoke-Msiexec -Leaves NodeSelected -Operation '/i' -Package $pace -Log 'after-retired.log' -What 'step 4: installing Pace once the retired product is gone'
Assert-ServiceTable -Row NodeSelected -Log 'after-retired.log'
Invoke-Msiexec -Leaves NothingInstalled -Operation '/x' -Package $pace -Log 'after-retired-uninstall.log' -What 'step 5: uninstalling Pace'
Assert-InstalledProduct $family ''
Assert-ServiceTable -Row NothingInstalled -Log 'after-retired-uninstall.log'
Remove-PaceMachineState
