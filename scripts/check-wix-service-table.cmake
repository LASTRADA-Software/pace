# SPDX-License-Identifier: Apache-2.0
#
# The service table in packaging/windows/service-actions.xml, asserted element by
# element.
#
# The MSI is the one artefact here whose behaviour only a Windows runner can
# exercise, minutes into the slowest job in the matrix, and a condition that lost a
# clause still builds a package. Two of the clauses are guards whose absence would
# do damage rather than fail: the leftover removal deletes a SERVICE, so it must be
# keyed on an upgrade, on the feature being gone, and on the registration pointing
# into this install root (a service registered by hand from another directory is
# not the installer's to delete). This check pins those clauses in the source,
# where a change to them is a red test on every platform.
#
# What it asserts:
#   1. every row of the table below: the element opening with `opener` exists
#      exactly once, and the text up to its `terminator` contains `required`
#      (entities decoded), or, for a `required` starting with `!`, does NOT;
#   2. every Condition attribute that reads one of the 0-then-1 flags (which
#      features are left installed, whether a registration is ours) compares it
#      with "1". Those properties are written 0 and then 1, so a bare truth test
#      reads "0" as true.
#
# The After= rows also pin the ONE sequence: validate, register, remember, start,
# and only then delete. The node's arguments are CHECKED first, directly after the
# files and the registry values (PaceNodeCheckArguments, Return="check"), so a fleet pin its parser
# refuses fails the transaction with nothing changed (B3-1, and B4-3: the leftover
# deletions once ran before it, and nothing restores a deleted service, so they are
# LAST now); the registration and the waiting start are Return="check" (ci-fix2);
# pace-kv stopped for the node is waited for until its PROCESS exits, and an
# upgrade waits for the old product's service processes before its files (B4-2);
# and every step a later failure would orphan has its ROLLBACK twin scheduled just
# before it -- a registration this transaction created is removed, one it
# re-applied is put back exactly from a copy of its key (B4-1, B4-4), a service it
# stopped while running is started again, and each remembered value is written
# back or deleted as it was found -- so a failed step leaves nothing behind.
#
# The MSIRESTARTMANAGERCONTROL row pins Restart Manager OUT of every transaction ("Disable"): a
# silent install otherwise restarts, at its end, every service it shut down at InstallValidate,
# whatever the service table decided. PR 1634's CI measured FastCached started after the table had
# made it manual and stopped it for the node, and then MEASURED whose session did it: the OLD
# package's removal, nested in the upgrade, while this package's own logged "Disabled". So this row is
# necessary and not sufficient; the PaceDisableForNode rows below are the other half (see the
# comment on MSIRESTARTMANAGERCONTROL). Its offence names that defect below, since the footer
# is generic. msi-custom-action-commands step 13 judges the same row, and refuses a custom action
# setting the property.
#
# The PaceDisableForNode rows pin pace-kv DISABLED beside the node, Return="check": an OLD
# package's removal inside an upgrade keeps its own Restart Manager session, and PR 1634's CI MEASURED
# Windows Installer restarting FastCached by name after the whole transaction had ended, onto the
# port the node holds. Disabled, that start fails at the service control manager with no process.
# And only a registration that EXISTS: the 1060 guard rows, since pace-kv's own registration is
# Return="ignore" and a checked disable of an absent service failed the node's whole install
# (rm-nested review, I-1). msi-custom-action-commands step 14 runs the command both ways.
#
# The PACE_INSTALL_LOCATION rows pin how a MAINTENANCE transaction resolves
# INSTALL_ROOT: from the product's OWN uninstall entry, when Installed, before
# CostFinalize. CPack resolves it only on an upgrade, and an uninstall of a custom
# root ran the default root's binary and left its service registered (CI run
# 37456507637). msi-custom-action-commands step 12 judges the same row by its shape.
#
# One row is a UNIQUENESS pin rather than an element: its opener is the bare
# `Before="InstallFinalize"`, so rule 1 requires that text exactly once, and the
# element it opens to be the forced failure's. PaceFailForTest is therefore the
# ONE action scheduled there, behind every other script action, which is what
# "a failure there rolls back everything the script did" rests on (#1629).
#
# What it does NOT see: whether Windows Installer evaluates a condition the way it
# reads, and anything about sequencing beyond the After= values pinned below. The
# `Package (Windows .msi)` job is the only thing that runs the package. This fails
# CLOSED on a row whose opener is missing or repeated, and OPEN on a clause that is
# present but wrong in a way no row names.
#
# Usage:
#   cmake -DPACE_SOURCE_DIR=<dir> -P scripts/check-wix-service-table.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED PACE_SOURCE_DIR)
    message(FATAL_ERROR "PACE_SOURCE_DIR must be set")
endif()

set(_file "${PACE_SOURCE_DIR}/packaging/windows/service-actions.xml")
if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "check-wix-service-table: ${_file} does not exist")
endif()

# One row per line: opener | terminator | required. ONE bracket argument walked
# line by line with FIND and SUBSTRING, never a CMake list: a row carries
# `[INSTALL_ROOT]`, and a square bracket or a `;` in a list element merges or
# drops elements in silence. A blank line is a separator and is skipped.
set(_table [=[
<Property Id="PACE_NODE_IMAGEPATH"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCacheCompileNode"
<Property Id="PACE_NODE_IMAGEPATH"|</Property>|Name="ImagePath"
<Property Id="PACE_NODE_IMAGEPATH"|</Property>|Root="HKLM"
<Property Id="PACE_IMAGEPATH"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCached"
<Property Id="PACE_IMAGEPATH"|</Property>|Name="ImagePath"
<Property Id="PACE_IMAGEPATH"|</Property>|Root="HKLM"

<Property Id="PACE_INSTALL_LOCATION"|</Property>|Root="HKLM"
<Property Id="PACE_INSTALL_LOCATION"|</Property>|Key="Software\Microsoft\Windows\CurrentVersion\Uninstall\[ProductCode]"
<Property Id="PACE_INSTALL_LOCATION"|</Property>|Name="InstallLocation"
<Property Id="PACE_INSTALL_LOCATION"|</Property>|Bitness="always64"
<Property Id="PACE_INSTALL_LOCATION"|</Property>|Type="raw"
<SetProperty Action="SetPaceInstallRootFromInstallLocation"|/>|Id="INSTALL_ROOT"
<SetProperty Action="SetPaceInstallRootFromInstallLocation"|/>|Value="[PACE_INSTALL_LOCATION]"
<SetProperty Action="SetPaceInstallRootFromInstallLocation"|/>|Before="CostFinalize" Sequence="both"
<SetProperty Action="SetPaceInstallRootFromInstallLocation"|/>|Condition="Installed AND PACE_INSTALL_LOCATION"

<Property Id="MSIRESTARTMANAGERCONTROL"|/>|Value="Disable"

<CustomAction Id="PaceNodeDeleteLeftover"|/>|sc.exe" delete FastCacheCompileNode"
<CustomAction Id="PaceNodeDeleteLeftover"|/>|Execute="deferred"
<CustomAction Id="PaceNodeDeleteLeftover"|/>|Impersonate="no"
<CustomAction Id="PaceNodeDeleteLeftover"|/>|Return="ignore"
<CustomAction Id="PaceDeleteLeftover"|/>|sc.exe" delete FastCached"
<CustomAction Id="PaceDeleteLeftover"|/>|Execute="deferred"
<CustomAction Id="PaceDeleteLeftover"|/>|Impersonate="no"
<CustomAction Id="PaceDeleteLeftover"|/>|Return="ignore"

<Custom Action="PaceNodeDeleteLeftover"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="PaceNodeDeleteLeftover"|/>|NOT UPGRADINGPRODUCTCODE
<Custom Action="PaceNodeDeleteLeftover"|/>|NOT (PACE_NODE_SELECTED = "1")
<Custom Action="PaceNodeDeleteLeftover"|/>|PACE_NODE_REGISTERED_HERE = "1"
<Custom Action="PaceNodeDeleteLeftover"|/>|!~><
<Custom Action="PaceNodeDeleteLeftover"|/>|After="PaceDiscardRollbackState"
<Custom Action="PaceDeleteLeftover"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="PaceDeleteLeftover"|/>|NOT UPGRADINGPRODUCTCODE
<Custom Action="PaceDeleteLeftover"|/>|NOT (PACE_SELECTED = "1")
<Custom Action="PaceDeleteLeftover"|/>|PACE_REGISTERED_HERE = "1"
<Custom Action="PaceDeleteLeftover"|/>|!~><
<Custom Action="PaceDeleteLeftover"|/>|After="PaceNodeDeleteLeftoverFirewall"
<CustomAction Id="PaceNodeDeleteLeftoverFirewall"|/>|Remove-NetFirewallRule -Group 'Pace: FastCacheCompileNode'
<CustomAction Id="PaceNodeDeleteLeftoverFirewall"|/>|Execute="deferred"
<CustomAction Id="PaceNodeDeleteLeftoverFirewall"|/>|Impersonate="no"
<CustomAction Id="PaceNodeDeleteLeftoverFirewall"|/>|Return="ignore"
<CustomAction Id="PaceDeleteLeftoverFirewall"|/>|Remove-NetFirewallRule -Group 'Pace: FastCached'
<CustomAction Id="PaceDeleteLeftoverFirewall"|/>|Execute="deferred"
<CustomAction Id="PaceDeleteLeftoverFirewall"|/>|Impersonate="no"
<CustomAction Id="PaceDeleteLeftoverFirewall"|/>|Return="ignore"
<Custom Action="PaceNodeDeleteLeftoverFirewall"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="PaceNodeDeleteLeftoverFirewall"|/>|NOT (PACE_NODE_SELECTED = "1")
<Custom Action="PaceNodeDeleteLeftoverFirewall"|/>|PACE_NODE_REGISTERED_HERE = "1"
<Custom Action="PaceNodeDeleteLeftoverFirewall"|/>|After="PaceNodeDeleteLeftover"
<Custom Action="PaceDeleteLeftoverFirewall"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="PaceDeleteLeftoverFirewall"|/>|NOT (PACE_SELECTED = "1")
<Custom Action="PaceDeleteLeftoverFirewall"|/>|PACE_REGISTERED_HERE = "1"
<Custom Action="PaceDeleteLeftoverFirewall"|/>|After="PaceDeleteLeftover"
<Custom Action="PaceSeedConfig"|/>|After="PaceNodeCheckArguments"
<CustomAction Id="PaceNodeInstallService"|/>|!PaceNodeAdvertiseArgument
<CustomAction Id="PaceNodeCheckArguments"|/>|!PaceNodeAdvertiseArgument
<CustomAction Id="PaceNodeRestoreRegistration"|/>|!PaceNodeAdvertise
<CustomAction Id="PaceForgetNodeAdvertise"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v NodeAdvertise /f /reg:64"
<CustomAction Id="PaceForgetNodeAdvertise"|/>|Execute="deferred"
<CustomAction Id="PaceForgetNodeAdvertise"|/>|Impersonate="no"
<CustomAction Id="PaceForgetNodeAdvertise"|/>|Return="ignore"
<CPackWiXFragment Id="#PRODUCT">|</CPackWiXFragment>|!PACE_NODE_ADVERTISE
<Custom Action="PaceForgetNodeAdvertise"|/>|Condition="PACE_SELECTED = "1" OR PACE_NODE_SELECTED = "1""
<Custom Action="PaceForgetNodeAdvertise"|/>|After="PaceRememberFleetId"

<CustomAction Id="PaceNodeInstallService"|/>|!--discovery-reply-port=
<CustomAction Id="PaceNodeInstallService"|/>|!--scheduler
<CustomAction Id="PaceNodeInstallService"|/>|Return="check"
<CustomAction Id="PaceNodeStartService"|/>|Return="check"
<CustomAction Id="PaceNodeStartService"|/>|/c [System64Folder]net.exe start FastCacheCompileNode && [System64Folder]sc.exe query FastCacheCompileNode | [System64Folder]find.exe "RUNNING""
<CustomAction Id="PaceNodeStartService"|/>|!ping.exe
<CustomAction Id="PaceNodeStartService"|/>|!||
<CustomAction Id="PaceNodeStartService"|/>|!sc.exe" start
<CustomAction Id="PaceInstallService"|/>|!DiscoveryReply
<Property Id="PACE_DISCOVERY_REPLY_PORT"|/>|Value="6682"
<CustomAction Id="PaceInstallService"|/>|--service-start=[PACE_START_MODE] [PaceFirewallAllowArgument]"
<CustomAction Id="PaceInstallService"|/>|!--firewall-allow=
<CustomAction Id="PaceNodeInstallService"|/>|!--firewall-allow=
<CustomAction Id="PaceNodeInstallService"|/>|!--cluster-dir
<CustomAction Id="PaceNodeInstallService"|/>|!--advertise=
<CustomAction Id="PaceNodeInstallService"|/>|!--fleet-seed=
<CustomAction Id="PaceNodeInstallService"|/>|!--fleet-id=
<Custom Action="PaceNodeInstallService"|/>|Condition="PACE_NODE_SELECTED = "1""
<Custom Action="PaceNodeInstallService"|/>|!PACE_NODE_SCHEDULER
<Custom Action="PaceNodeInstallService"|/>|!PACE_NODE_ADVERTISE
<Custom Action="PaceNodeInstallService"|/>|!PACE_FLEET_SEED
<Custom Action="PaceNodeInstallService"|/>|!PACE_FLEET_ID
<SetProperty Action="SetPaceFirewallAllowArgument"|/>|Id="PaceFirewallAllowArgument"
<SetProperty Action="SetPaceFirewallAllowArgument"|/>|Value="--firewall-allow=[PACE_FIREWALL_ALLOW]"
<SetProperty Action="SetPaceFirewallAllowArgument"|/>|Condition="PACE_FIREWALL_ALLOW"
<SetProperty Action="SetPaceNodeDiscoveryReplyArgument"|/>|Id="PaceNodeDiscoveryReplyArgument"
<SetProperty Action="SetPaceNodeDiscoveryReplyArgument"|/>|Value="--discovery-reply-port=[PACE_DISCOVERY_REPLY_PORT]"
<SetProperty Action="SetPaceNodeDiscoveryReplyArgument"|/>|Condition="PACE_DISCOVERY_REPLY_PORT"
<SetProperty Action="SetPaceNodeDiscoveryReplyArgument"|/>|After="SetPaceFirewallAllowArgument"
<SetProperty Action="SetPaceFleetSeedArgument"|/>|Id="PaceFleetSeedArgument"
<SetProperty Action="SetPaceFleetSeedArgument"|/>|Value="--fleet-seed=[PACE_FLEET_SEED]"
<SetProperty Action="SetPaceFleetSeedArgument"|/>|Condition="PACE_FLEET_SEED"
<SetProperty Action="SetPaceFleetSeedArgument"|/>|After="SetPaceNodeDiscoveryReplyArgument"
<SetProperty Action="SetPaceFleetIdArgument"|/>|Id="PaceFleetIdArgument"
<SetProperty Action="SetPaceFleetIdArgument"|/>|Value="--fleet-id=[PACE_FLEET_ID]"
<SetProperty Action="SetPaceFleetIdArgument"|/>|Condition="PACE_FLEET_ID"
<SetProperty Action="SetPaceFleetIdArgument"|/>|After="SetPaceFleetSeedArgument"

<Property Id="PACE_FIREWALL_ALLOW"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="PACE_FIREWALL_ALLOW"|</Property>|Name="FirewallAllow"
<Property Id="PACE_FIREWALL_ALLOW"|</Property>|Root="HKLM"
<Property Id="PACE_FLEET_SEED"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="PACE_FLEET_SEED"|</Property>|Name="FleetSeed"
<Property Id="PACE_FLEET_SEED"|</Property>|Root="HKLM"
<Property Id="PACE_FLEET_ID"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="PACE_FLEET_ID"|</Property>|Name="FleetId"
<Property Id="PACE_FLEET_ID"|</Property>|Root="HKLM"
<SetProperty Action="SaveTypedPaceFirewallAllow"|/>|Value="[PACE_FIREWALL_ALLOW]"
<SetProperty Action="SaveTypedPaceFirewallAllow"|/>|Before="AppSearch" Sequence="both"
<SetProperty Action="SaveTypedPaceFirewallAllow"|/>|Condition="PACE_FIREWALL_ALLOW"
<SetProperty Action="RestoreTypedPaceFirewallAllow"|/>|Id="PACE_FIREWALL_ALLOW"
<SetProperty Action="RestoreTypedPaceFirewallAllow"|/>|After="AppSearch" Sequence="both"
<SetProperty Action="RestoreTypedPaceFirewallAllow"|/>|Condition="PaceFirewallAllowTyped"
<SetProperty Action="SaveTypedPaceFleetSeed"|/>|Value="[PACE_FLEET_SEED]"
<SetProperty Action="SaveTypedPaceFleetSeed"|/>|Before="AppSearch" Sequence="both"
<SetProperty Action="SaveTypedPaceFleetSeed"|/>|Condition="PACE_FLEET_SEED"
<SetProperty Action="RestoreTypedPaceFleetSeed"|/>|Id="PACE_FLEET_SEED"
<SetProperty Action="RestoreTypedPaceFleetSeed"|/>|After="AppSearch" Sequence="both"
<SetProperty Action="RestoreTypedPaceFleetSeed"|/>|Condition="PaceFleetSeedTyped"
<SetProperty Action="SaveTypedPaceFleetId"|/>|Value="[PACE_FLEET_ID]"
<SetProperty Action="SaveTypedPaceFleetId"|/>|Before="AppSearch" Sequence="both"
<SetProperty Action="SaveTypedPaceFleetId"|/>|Condition="PACE_FLEET_ID"
<SetProperty Action="RestoreTypedPaceFleetId"|/>|Id="PACE_FLEET_ID"
<SetProperty Action="RestoreTypedPaceFleetId"|/>|After="AppSearch" Sequence="both"
<SetProperty Action="RestoreTypedPaceFleetId"|/>|Condition="PaceFleetIdTyped"
<Component Id="PaceRememberedProperties"|</Component>|Guid="87BB5EEE-8B9E-4401-9367-C86DB5061258"
<Component Id="PaceRememberedProperties"|</Component>|Action="removeOnUninstall"
<ComponentRef Id="PaceRememberedProperties"|/>|ComponentRef
<CustomAction Id="PaceRememberFirewallAllow"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FirewallAllow /t REG_SZ /d "[PACE_FIREWALL_ALLOW]" /f /reg:64"
<CustomAction Id="PaceRememberFirewallAllow"|/>|Execute="deferred"
<CustomAction Id="PaceRememberFirewallAllow"|/>|Impersonate="no"
<CustomAction Id="PaceRememberFirewallAllow"|/>|Return="check"
<CustomAction Id="PaceRememberFleetSeed"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetSeed /t REG_SZ /d "[PACE_FLEET_SEED]" /f /reg:64"
<CustomAction Id="PaceRememberFleetSeed"|/>|Execute="deferred"
<CustomAction Id="PaceRememberFleetSeed"|/>|Impersonate="no"
<CustomAction Id="PaceRememberFleetSeed"|/>|Return="check"
<CustomAction Id="PaceRememberFleetId"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetId /t REG_SZ /d "[PACE_FLEET_ID]" /f /reg:64"
<CustomAction Id="PaceRememberFleetId"|/>|Execute="deferred"
<CustomAction Id="PaceRememberFleetId"|/>|Impersonate="no"
<CustomAction Id="PaceRememberFleetId"|/>|Return="check"
<CustomAction Id="PaceNodeCheckArguments"|/>|Execute="deferred"
<CustomAction Id="PaceNodeCheckArguments"|/>|Impersonate="no"
<CustomAction Id="PaceNodeCheckArguments"|/>|Return="check"
<Custom Action="PaceNodeCheckArguments"|/>|Condition="PACE_NODE_SELECTED = "1""
<Custom Action="PaceNodeCheckArguments"|/>|After="PaceNodeRestoreRegistration"
<Custom Action="PaceRememberFirewallAllow"|/>|Condition="PACE_SELECTED = "1" OR PACE_NODE_SELECTED = "1""
<Custom Action="PaceRememberFleetSeed"|/>|Condition="PACE_NODE_SELECTED = "1""
<Custom Action="PaceRememberFleetSeed"|/>|!PACE_SELECTED
<Custom Action="PaceRememberFleetId"|/>|Condition="PACE_NODE_SELECTED = "1""
<Custom Action="PaceRememberFleetId"|/>|!PACE_SELECTED
<Property Id="PACE_FIREWALL_ALLOW_BEFORE"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="PACE_FIREWALL_ALLOW_BEFORE"|</Property>|Name="FirewallAllow"
<CustomAction Id="PaceRollbackRestoreFirewallAllow"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FirewallAllow /t REG_SZ /d "[PACE_FIREWALL_ALLOW_BEFORE]" /f /reg:64"
<CustomAction Id="PaceRollbackRestoreFirewallAllow"|/>|Execute="rollback"
<CustomAction Id="PaceRollbackDeleteFirewallAllow"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v FirewallAllow /f /reg:64"
<CustomAction Id="PaceRollbackDeleteFirewallAllow"|/>|Execute="rollback"
<Custom Action="PaceRollbackRestoreFirewallAllow"|/>|After="PaceNodeInstallService"
<Custom Action="PaceRollbackRestoreFirewallAllow"|/>|Condition="(PACE_SELECTED = "1" OR PACE_NODE_SELECTED = "1") AND PACE_FIREWALL_ALLOW_BEFORE"
<Custom Action="PaceRollbackDeleteFirewallAllow"|/>|After="PaceRollbackRestoreFirewallAllow"
<Custom Action="PaceRollbackDeleteFirewallAllow"|/>|Condition="(PACE_SELECTED = "1" OR PACE_NODE_SELECTED = "1") AND NOT PACE_FIREWALL_ALLOW_BEFORE"
<Custom Action="PaceRememberFirewallAllow"|/>|After="PaceRollbackDeleteFirewallAllow"
<Property Id="PACE_FLEET_SEED_BEFORE"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="PACE_FLEET_SEED_BEFORE"|</Property>|Name="FleetSeed"
<CustomAction Id="PaceRollbackRestoreFleetSeed"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetSeed /t REG_SZ /d "[PACE_FLEET_SEED_BEFORE]" /f /reg:64"
<CustomAction Id="PaceRollbackRestoreFleetSeed"|/>|Execute="rollback"
<CustomAction Id="PaceRollbackDeleteFleetSeed"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v FleetSeed /f /reg:64"
<CustomAction Id="PaceRollbackDeleteFleetSeed"|/>|Execute="rollback"
<Custom Action="PaceRollbackRestoreFleetSeed"|/>|Condition="(PACE_NODE_SELECTED = "1") AND PACE_FLEET_SEED_BEFORE"
<Custom Action="PaceRollbackDeleteFleetSeed"|/>|After="PaceRollbackRestoreFleetSeed"
<Custom Action="PaceRollbackDeleteFleetSeed"|/>|Condition="(PACE_NODE_SELECTED = "1") AND NOT PACE_FLEET_SEED_BEFORE"
<Custom Action="PaceRememberFleetSeed"|/>|After="PaceRollbackDeleteFleetSeed"
<Property Id="PACE_FLEET_ID_BEFORE"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="PACE_FLEET_ID_BEFORE"|</Property>|Name="FleetId"
<CustomAction Id="PaceRollbackRestoreFleetId"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetId /t REG_SZ /d "[PACE_FLEET_ID_BEFORE]" /f /reg:64"
<CustomAction Id="PaceRollbackRestoreFleetId"|/>|Execute="rollback"
<CustomAction Id="PaceRollbackDeleteFleetId"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v FleetId /f /reg:64"
<CustomAction Id="PaceRollbackDeleteFleetId"|/>|Execute="rollback"
<Custom Action="PaceRollbackRestoreFleetId"|/>|After="PaceRememberFleetSeed"
<Custom Action="PaceRollbackRestoreFleetId"|/>|Condition="(PACE_NODE_SELECTED = "1") AND PACE_FLEET_ID_BEFORE"
<Custom Action="PaceRollbackDeleteFleetId"|/>|After="PaceRollbackRestoreFleetId"
<Custom Action="PaceRollbackDeleteFleetId"|/>|Condition="(PACE_NODE_SELECTED = "1") AND NOT PACE_FLEET_ID_BEFORE"
<Custom Action="PaceRememberFleetId"|/>|After="PaceRollbackDeleteFleetId"
<CustomAction Id="PaceUndoRegistration"|/>|pace-kv.exe" --uninstall-service"
<CustomAction Id="PaceUndoRegistration"|/>|Execute="rollback"
<Custom Action="PaceUndoRegistration"|/>|After="PaceRestoreRegistrationExactly"
<Custom Action="PaceUndoRegistration"|/>|Condition="PACE_SELECTED = "1" AND NOT PACE_IMAGEPATH"
<Custom Action="PaceInstallService"|/>|After="PaceNodeSeedConfig"
<CustomAction Id="PaceNodeUndoRegistration"|/>|pace-node.exe" --uninstall-service"
<CustomAction Id="PaceNodeUndoRegistration"|/>|Execute="rollback"
<Custom Action="PaceNodeUndoRegistration"|/>|After="PaceNodeRestoreRegistrationExactly"
<Custom Action="PaceNodeUndoRegistration"|/>|Condition="PACE_NODE_SELECTED = "1" AND NOT PACE_NODE_IMAGEPATH"
<CustomAction Id="PaceNodeRestoreRegistration"|/>|Execute="rollback"
<Custom Action="PaceNodeRestoreRegistration"|/>|After="PaceNodeUndoRegistration"
<Custom Action="PaceNodeRestoreRegistration"|/>|Condition="PACE_NODE_SELECTED = "1" AND PACE_NODE_IMAGEPATH"
<Custom Action="PaceNodeInstallService"|/>|After="PaceStartService"
<Custom Action="PaceNodeStartService"|/>|After="PaceNodeStopForRestart"

<SetProperty Action="SetPaceOwnImagePathPrefix"|/>|Value=""[INSTALL_ROOT]"
<SetProperty Action="SetPaceOwnImagePathPrefix"|/>|After="CostFinalize"
<SetProperty Action="SetPaceOwnImagePathPrefix"|/>|!Condition=
<SetProperty Action="SetPaceOwnImagePathRawPrefix"|/>|Value="#%"[INSTALL_ROOT]"
<SetProperty Action="SetPaceOwnImagePathRawPrefix"|/>|After="SetPaceOwnImagePathPrefix"
<SetProperty Action="SetPaceOwnImagePathRawPrefix"|/>|!Condition=

<SetProperty Action="ClearPaceNodeRegisteredHere"|/>|Value="0"
<SetProperty Action="ClearPaceNodeRegisteredHere"|/>|After="SetPaceOwnImagePathRawPrefix"
<SetProperty Action="ClearPaceNodeRegisteredHere"|/>|!Condition=
<SetProperty Action="SetPaceNodeRegisteredHereQuoted"|/>|Condition="PACE_OWN_IMAGEPATH_PREFIX AND PACE_NODE_IMAGEPATH ~<< PACE_OWN_IMAGEPATH_PREFIX"
<SetProperty Action="SetPaceNodeRegisteredHereQuoted"|/>|After="ClearPaceNodeRegisteredHere"
<SetProperty Action="SetPaceNodeRegisteredHereRaw"|/>|Condition="PACE_OWN_IMAGEPATH_RAW_PREFIX AND PACE_NODE_IMAGEPATH ~<< PACE_OWN_IMAGEPATH_RAW_PREFIX"
<SetProperty Action="SetPaceNodeRegisteredHereRaw"|/>|After="SetPaceNodeRegisteredHereQuoted"
<SetProperty Action="ClearPaceRegisteredHere"|/>|Value="0"
<SetProperty Action="ClearPaceRegisteredHere"|/>|After="SetPaceNodeRegisteredHereRaw"
<SetProperty Action="ClearPaceRegisteredHere"|/>|!Condition=
<SetProperty Action="SetPaceRegisteredHereQuoted"|/>|Condition="PACE_OWN_IMAGEPATH_PREFIX AND PACE_IMAGEPATH ~<< PACE_OWN_IMAGEPATH_PREFIX"
<SetProperty Action="SetPaceRegisteredHereQuoted"|/>|After="ClearPaceRegisteredHere"
<SetProperty Action="SetPaceRegisteredHereRaw"|/>|Condition="PACE_OWN_IMAGEPATH_RAW_PREFIX AND PACE_IMAGEPATH ~<< PACE_OWN_IMAGEPATH_RAW_PREFIX"
<SetProperty Action="SetPaceRegisteredHereRaw"|/>|After="SetPaceRegisteredHereQuoted"

<SetProperty Action="ClearPaceNodeSelected"|/>|Value="0"
<SetProperty Action="ClearPaceNodeSelected"|/>|After="MigrateFeatureStates"
<SetProperty Action="ClearPaceNodeSelected"|/>|!Condition=
<SetProperty Action="SetPaceNodeSelected"|/>|After="ClearPaceNodeSelected"
<SetProperty Action="ClearPaceSelected"|/>|Value="0"
<SetProperty Action="ClearPaceSelected"|/>|After="SetPaceNodeSelected"
<SetProperty Action="ClearPaceSelected"|/>|!Condition=
<SetProperty Action="SetPaceSelected"|/>|After="ClearPaceSelected"

<Custom Action="PaceNodeUninstallService"|/>|NOT UPGRADINGPRODUCTCODE

<CustomAction Id="PaceAwaitServiceExit"|/>|bin\pace-kv.exe\"
<CustomAction Id="PaceAwaitServiceExit"|/>|bin\pace-node.exe\"
<CustomAction Id="PaceAwaitServiceExit"|/>|exit 1460
<CustomAction Id="PaceAwaitServiceExit"|/>|Execute="deferred"
<CustomAction Id="PaceAwaitServiceExit"|/>|Impersonate="no"
<CustomAction Id="PaceAwaitServiceExit"|/>|Return="check"
<CustomAction Id="PaceAwaitServiceExit"|/>|Directory="System64Folder"
<CustomAction Id="PaceAwaitServiceExit"|/>|!Directory="INSTALL_ROOT"
<Custom Action="PaceAwaitServiceExit"|/>|Before="InstallFiles"
<Custom Action="PaceAwaitServiceExit"|/>|Condition="WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE"
<CustomAction Id="PaceAwaitServiceExit"|/>|# PaceImagePath:[PACE_IMAGEPATH] PaceNodeImagePath:[PACE_NODE_IMAGEPATH]"
<CustomAction Id="PaceAwaitServiceExit"|/>|(Get-CimInstance Win32_Process -Filter \"ProcessId=$PID\").CommandLine
<CustomAction Id="PaceAwaitServiceExit"|/>|$p+=\"$d\pace-kv.exe\",\"$d\pace-node.exe\"
<CustomAction Id="PaceAwaitExitForNode"|/>|# PaceImagePath:[PACE_IMAGEPATH] PaceNodeImagePath:[PACE_NODE_IMAGEPATH]"
<CustomAction Id="PaceAwaitExitForNode"|/>|(Get-CimInstance Win32_Process -Filter \"ProcessId=$PID\").CommandLine
<CustomAction Id="PaceAwaitExitForNode"|/>|$p+=\"$d\pace-kv.exe\"
<CustomAction Id="PaceAwaitExitForNode"|/>|bin\pace-kv.exe\"
<CustomAction Id="PaceAwaitExitForNode"|/>|!pace-node
<CustomAction Id="PaceAwaitExitForNode"|/>|exit 1460
<CustomAction Id="PaceAwaitExitForNode"|/>|Return="check"
<Custom Action="PaceAwaitExitForNode"|/>|After="PaceStopForNode"
<Custom Action="PaceAwaitExitForNode"|/>|Condition="PACE_SELECTED = "1" AND PACE_NODE_SELECTED = "1""
<CustomAction Id="PaceStopForNode"|/>|/v PaceWasRunning
<CustomAction Id="PaceStopForNode"|/>|net.exe stop FastCached"
<CustomAction Id="PaceStopForNode"|/>|Return="ignore"
<Custom Action="PaceStopForNode"|/>|After="PaceInstallService"
<CustomAction Id="PaceDisableForNode"|/>|cmd.exe" /c ([System64Folder]sc.exe query FastCached >nul 2>&1 & if errorlevel 1060 if not errorlevel 1061 exit 0) &
<CustomAction Id="PaceDisableForNode"|/>|& [System64Folder]sc.exe config FastCached start= disabled"
<CustomAction Id="PaceDisableForNode"|/>|Execute="deferred"
<CustomAction Id="PaceDisableForNode"|/>|Impersonate="no"
<CustomAction Id="PaceDisableForNode"|/>|Return="check"
<Custom Action="PaceDisableForNode"|/>|After="PaceAwaitExitForNode"
<Custom Action="PaceDisableForNode"|/>|Condition="PACE_SELECTED = "1" AND PACE_NODE_SELECTED = "1""
<Custom Action="PaceStartService"|/>|After="PaceDisableForNode"

<CustomAction Id="PaceClearRollbackState"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\InstallerRollback /f /reg:64"
<CustomAction Id="PaceClearRollbackState"|/>|Execute="deferred"
<Custom Action="PaceClearRollbackState"|/>|Before="PaceUndoRollbackState"
<CustomAction Id="PaceClearRollbackState"|/>|Directory="System64Folder"
<CustomAction Id="PaceDiscardRollbackState"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\InstallerRollback /f /reg:64"
<CustomAction Id="PaceDiscardRollbackState"|/>|Execute="commit"
<CustomAction Id="PaceDiscardRollbackState"|/>|Directory="System64Folder"
<CustomAction Id="PaceFailForTest"|/>|Execute="deferred"
<CustomAction Id="PaceFailForTest"|/>|Return="check"
<CustomAction Id="PaceFailForTest"|/>|exit 1603
<CustomAction Id="PaceFailForTest"|/>|Impersonate="no"
<Custom Action="PaceFailForTest"|/>|Before="InstallFinalize"
Before="InstallFinalize"|/>|PACE_FAIL_FOR_TEST = "1"
<Custom Action="PaceFailForTest"|/>|PACE_FAIL_FOR_TEST = "1"
<Property Id="PACE_FAIL_FOR_TEST"|/>|Secure="yes"
<Custom Action="PaceDiscardRollbackState"|/>|After="PaceNodeStartService"
<CustomAction Id="PaceRestartAfterStop"|/>|/v PaceWasRunning
<CustomAction Id="PaceRestartAfterStop"|/>|sc.exe start FastCached"
<CustomAction Id="PaceRestartAfterStop"|/>|Execute="rollback"
<Custom Action="PaceRestartAfterStop"|/>|Before="PaceNodeRestartAfterStop"
<CustomAction Id="PaceRestartAfterStop"|/>|Directory="System64Folder"
<Custom Action="PaceRestartAfterStop"|/>|Condition="PACE_SELECTED = "1" AND PACE_NODE_SELECTED = "1""
<CustomAction Id="PaceNodeRestartAfterStop"|/>|/v NodeWasRunning
<CustomAction Id="PaceNodeRestartAfterStop"|/>|sc.exe start FastCacheCompileNode"
<CustomAction Id="PaceNodeRestartAfterStop"|/>|Execute="rollback"
<Custom Action="PaceNodeRestartAfterStop"|/>|Before="PaceAwaitServiceExit"
<CustomAction Id="PaceNodeRestartAfterStop"|/>|Directory="System64Folder"
<Custom Action="PaceUndoRollbackState"|/>|Before="PaceRestartAfterStop"
<Custom Action="PaceNodeRestartAfterStop"|/>|Condition="PACE_NODE_SELECTED = "1""
<CustomAction Id="PaceNodeStopForRestart"|/>|/v NodeWasRunning
<CustomAction Id="PaceNodeStopForRestart"|/>|net.exe stop FastCacheCompileNode"
<CustomAction Id="PaceNodeStopForRestart"|/>|Return="ignore"
<Custom Action="PaceNodeStopForRestart"|/>|After="PaceForgetNodeAdvertise"
<Custom Action="PaceNodeStopForRestart"|/>|Condition="PACE_NODE_SELECTED = "1""

<Property Id="PACE_START_BEFORE"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCached"
<Property Id="PACE_START_BEFORE"|</Property>|Name="Start"
<Property Id="PACE_NODE_START_BEFORE"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCacheCompileNode"
<Property Id="PACE_NODE_START_BEFORE"|</Property>|Name="Start"
<SetProperty Action="SetPaceStartModeBeforeAuto"|/>|Condition="PACE_START_BEFORE = "#2""
<SetProperty Action="SetPaceStartModeBeforeManual"|/>|Condition="NOT (PACE_START_BEFORE = "#2")"
<SetProperty Action="SetPaceNodeStartModeBeforeAuto"|/>|Condition="PACE_NODE_START_BEFORE = "#2""
<SetProperty Action="SetPaceNodeStartModeBeforeManual"|/>|Condition="NOT (PACE_NODE_START_BEFORE = "#2")"

<CustomAction Id="PaceStashRegistration"|/>|reg.exe" copy HKLM\SYSTEM\CurrentControlSet\Services\FastCached HKLM\SOFTWARE\fastcached\InstallerRollback\FastCached /s /f /reg:64"
<CustomAction Id="PaceStashFromCopy"|/>|reg.exe query HKLM\SYSTEM\CurrentControlSet\Services\FastCached /reg:64 >nul 2>&1 || [System64Folder]reg.exe copy HKLM\SOFTWARE\fastcached\InstallerRegistrationCopy\FastCached HKLM\SOFTWARE\fastcached\InstallerRollback\FastCached /s /f /reg:64)"
<CustomAction Id="PaceStashRegistration"|/>|Execute="deferred"
<Custom Action="PaceStashRegistration"|/>|After="WriteRegistryValues"
<Custom Action="PaceStashFromCopy"|/>|After="PaceStashRegistration"
<Custom Action="PaceStashFromCopy"|/>|AND WIX_UPGRADE_DETECTED"
<CustomAction Id="PaceStashRegistration"|/>|Directory="System64Folder"
<Custom Action="PaceStashRegistration"|/>|Condition="PACE_SELECTED = "1" AND PACE_IMAGEPATH"
<CustomAction Id="PaceRestoreRegistrationExactly"|/>|Rollback\FastCached'
<CustomAction Id="PaceRestoreRegistrationExactly"|/>|Invoke-CimMethod
<CustomAction Id="PaceRestoreRegistrationExactly"|/>|sc.exe config FastCached start=
<CustomAction Id="PaceRestoreRegistrationExactly"|/>|Execute="rollback"
<Custom Action="PaceRestoreRegistrationExactly"|/>|After="PaceStashFromCopy"
<Custom Action="PaceRestoreRegistrationExactly"|/>|Condition="PACE_SELECTED = "1" AND PACE_IMAGEPATH"
<CustomAction Id="PaceRestoreRegistration"|/>|pace-kv.exe" --install-service --service-start=[PaceStartModeBefore] [PaceFirewallAllowBeforeArgument]"
<CustomAction Id="PaceRestoreRegistration"|/>|Execute="rollback"
<Custom Action="PaceRestoreRegistration"|/>|After="PaceUndoRegistration"
<Custom Action="PaceRestoreRegistration"|/>|Condition="PACE_SELECTED = "1" AND PACE_IMAGEPATH"
<CustomAction Id="PaceNodeStashRegistration"|/>|reg.exe" copy HKLM\SYSTEM\CurrentControlSet\Services\FastCacheCompileNode HKLM\SOFTWARE\fastcached\InstallerRollback\FastCacheCompileNode /s /f /reg:64"
<CustomAction Id="PaceNodeStashFromCopy"|/>|reg.exe query HKLM\SYSTEM\CurrentControlSet\Services\FastCacheCompileNode /reg:64 >nul 2>&1 || [System64Folder]reg.exe copy HKLM\SOFTWARE\fastcached\InstallerRegistrationCopy\FastCacheCompileNode HKLM\SOFTWARE\fastcached\InstallerRollback\FastCacheCompileNode /s /f /reg:64)"
<Custom Action="PaceNodeStashRegistration"|/>|After="PaceRestoreRegistration"
<Custom Action="PaceNodeStashFromCopy"|/>|After="PaceNodeStashRegistration"
<Custom Action="PaceNodeStashFromCopy"|/>|AND WIX_UPGRADE_DETECTED"
<Component Id="PaceNodeRegistrationCopy"|</Component>|Condition="PACE_NODE_IMAGEPATH"
<Component Id="PaceNodeRegistrationCopy"|</Component>|Value="[PACE_NODE_IMAGEPATH]"
<Component Id="PaceRegistrationCopy"|</Component>|Condition="PACE_IMAGEPATH"
<Component Id="PaceRegistrationCopy"|</Component>|Value="[PACE_IMAGEPATH]"
<CustomAction Id="PaceNodeStashRegistration"|/>|Directory="System64Folder"
<Custom Action="PaceNodeStashRegistration"|/>|Condition="PACE_NODE_SELECTED = "1" AND PACE_NODE_IMAGEPATH"
<CustomAction Id="PaceNodeRestoreRegistrationExactly"|/>|Rollback\FastCacheCompileNode'
<CustomAction Id="PaceNodeRestoreRegistrationExactly"|/>|sc.exe config FastCacheCompileNode start=
<CustomAction Id="PaceNodeRestoreRegistrationExactly"|/>|Execute="rollback"
<Custom Action="PaceNodeRestoreRegistrationExactly"|/>|After="PaceNodeStashFromCopy"
<Custom Action="PaceNodeRestoreRegistrationExactly"|/>|Condition="PACE_NODE_SELECTED = "1" AND PACE_NODE_IMAGEPATH"
<CustomAction Id="PaceNodeRestoreRegistration"|/>|--service-start=[PaceNodeStartModeBefore]
<CustomAction Id="PaceNodeRestoreRegistration"|/>|!--service-start=auto
<Custom Action="PaceUninstallService"|/>|NOT UPGRADINGPRODUCTCODE
]=])

# The 0-then-1 flags rule 2 is about. Each must be read by at least one condition, or
# the rule is vacuous for it.
set(_flagProperties PACE_NODE_SELECTED PACE_SELECTED PACE_NODE_REGISTERED_HERE
    PACE_REGISTERED_HERE)

file(READ "${_file}" _text)

# Entities are decoded per extracted piece, never over the whole file: a decoded
# `&quot;` would end the attribute value the walk below is delimiting.
function(_fc_decode _in _out)
    string(REPLACE "&quot;" "\"" _s "${_in}")
    string(REPLACE "&gt;" ">" _s "${_s}")
    string(REPLACE "&lt;" "<" _s "${_s}")
    string(REPLACE "&amp;" "&" _s "${_s}")
    set(${_out} "${_s}" PARENT_SCOPE)
endfunction()

set(_offences "")
set(_rowCount 0)

set(_tableCursor 0)
string(LENGTH "${_table}" _tableLength)
while(_tableCursor LESS _tableLength)
    string(SUBSTRING "${_table}" ${_tableCursor} -1 _tableRest)
    string(FIND "${_tableRest}" "\n" _lineEnd)
    if(_lineEnd EQUAL -1)
        string(LENGTH "${_tableRest}" _lineEnd)
    endif()
    string(SUBSTRING "${_tableRest}" 0 ${_lineEnd} _row)
    math(EXPR _tableCursor "${_tableCursor} + ${_lineEnd} + 1")
    if(_row STREQUAL "")
        continue()
    endif()

    string(FIND "${_row}" "|" _bar1)
    if(_bar1 EQUAL -1)
        list(APPEND _offences "malformed table row (no |): ${_row}")
        continue()
    endif()
    string(SUBSTRING "${_row}" 0 ${_bar1} _opener)
    math(EXPR _after1 "${_bar1} + 1")
    string(SUBSTRING "${_row}" ${_after1} -1 _rest)
    string(FIND "${_rest}" "|" _bar2)
    if(_bar2 EQUAL -1)
        list(APPEND _offences "malformed table row (one |): ${_row}")
        continue()
    endif()
    string(SUBSTRING "${_rest}" 0 ${_bar2} _terminator)
    math(EXPR _after2 "${_bar2} + 1")
    string(SUBSTRING "${_rest}" ${_after2} -1 _required)
    math(EXPR _rowCount "${_rowCount} + 1")

    string(FIND "${_text}" "${_opener}" _first)
    string(FIND "${_text}" "${_opener}" _last REVERSE)
    if(_first EQUAL -1)
        list(APPEND _offences "missing: ${_opener}")
        continue()
    endif()
    if(NOT _first EQUAL _last)
        list(APPEND _offences "not unique: ${_opener}")
        continue()
    endif()

    string(SUBSTRING "${_text}" ${_first} -1 _fromOpener)
    string(FIND "${_fromOpener}" "${_terminator}" _end)
    if(_end EQUAL -1)
        list(APPEND _offences "never terminated by ${_terminator}: ${_opener}")
        continue()
    endif()
    string(SUBSTRING "${_fromOpener}" 0 ${_end} _element)
    _fc_decode("${_element}" _element)

    if(_required MATCHES "^!")
        string(SUBSTRING "${_required}" 1 -1 _forbidden)
        string(FIND "${_element}" "${_forbidden}" _hit)
        if(NOT _hit EQUAL -1)
            list(APPEND _offences "${_opener} must not carry: ${_forbidden}")
        endif()
    else()
        string(FIND "${_element}" "${_required}" _hit)
        if(_hit EQUAL -1)
            list(APPEND _offences "${_opener} lacks: ${_required}")
        endif()
    endif()
endwhile()

# Rule 2: a FIND/SUBSTRING walk over every Condition attribute, never a list split.
set(_conditions 0)
set(_readers "")
set(_cursor 0)
string(LENGTH "${_text}" _length)
while(_cursor LESS _length)
    string(SUBSTRING "${_text}" ${_cursor} -1 _tail)
    string(FIND "${_tail}" "Condition=\"" _at)
    if(_at EQUAL -1)
        break()
    endif()
    math(EXPR _valueStart "${_cursor} + ${_at} + 11")
    string(SUBSTRING "${_text}" ${_valueStart} -1 _valueTail)
    string(FIND "${_valueTail}" "\"" _valueEnd)
    if(_valueEnd EQUAL -1)
        list(APPEND _offences "a Condition attribute is never closed")
        break()
    endif()
    string(SUBSTRING "${_valueTail}" 0 ${_valueEnd} _condition)
    _fc_decode("${_condition}" _condition)
    math(EXPR _conditions "${_conditions} + 1")

    foreach(_property IN LISTS _flagProperties)
        set(_scan "${_condition}")
        while(TRUE)
            string(FIND "${_scan}" "${_property}" _p)
            if(_p EQUAL -1)
                break()
            endif()
            list(APPEND _readers "${_property}")
            string(LENGTH "${_property}" _nameLength)
            math(EXPR _next "${_p} + ${_nameLength}")
            string(SUBSTRING "${_scan}" ${_next} -1 _scan)
            string(FIND "${_scan}" " = \"1\"" _cmp)
            if(NOT _cmp EQUAL 0)
                list(APPEND _offences "a condition reads ${_property} without comparing it with \"1\": ${_condition}")
                break()
            endif()
        endwhile()
    endforeach()

    math(EXPR _cursor "${_valueStart} + ${_valueEnd} + 1")
endwhile()

# Positive controls: a walk that found nothing reports nothing wrong about it.
if(_conditions EQUAL 0)
    message(FATAL_ERROR
        "check-wix-service-table: found no Condition attribute in ${_file}, so rule 2 proved nothing rather than passing.")
endif()
foreach(_property IN LISTS _flagProperties)
    if(NOT _property IN_LIST _readers)
        list(APPEND _offences "no condition reads ${_property}, so rule 2 is vacuous for it")
    endif()
endforeach()

if(_offences)
    string(REPLACE ";" "\n  " _offenceText "${_offences}")
    # The one row whose offence names its defect, since the footer below is generic.
    set(_restartManagerText "")
    string(FIND "${_offenceText}" "MSIRESTARTMANAGERCONTROL" _restartManagerOffence)
    if(NOT _restartManagerOffence EQUAL -1)
        set(_restartManagerText
            "\nMSIRESTARTMANAGERCONTROL must be Disable: Windows Installer's Restart Manager restarts a service the "
            "table left stopped. A silent install starts, at its end, every service it shut down at InstallValidate, "
            "whatever the table decided, and that shutdown comes before StopServices, so the ServiceControl rows "
            "record no rollback start. See the property's comment in service-actions.xml.")
        string(CONCAT _restartManagerText ${_restartManagerText})
    endif()
    message(FATAL_ERROR
        "check-wix-service-table: packaging/windows/service-actions.xml no longer carries the service "
        "table this check pins.\n"
        "  ${_offenceText}\n"
        "If the change is deliberate, change the row in scripts/check-wix-service-table.cmake in the "
        "same commit and say why there. A leftover-removal row lost its guard means the MSI can delete "
        "a service it did not register.${_restartManagerText}")
endif()

message(STATUS
    "check-wix-service-table: ${_rowCount} row(s) and ${_conditions} condition(s) checked in service-actions.xml")
