# SPDX-License-Identifier: Apache-2.0
#
# The STARVATION WITNESS and the three-outcome ceiling verdict, for the PowerShell launcher fixture
# (#1739). The POSIX twin is `e2e_starvation_witness` and `e2e_dead_ceiling_verdict` in
# `scripts/lib/e2e-common.sh`; PowerShell cannot source that file, so the two are kept in step by
# hand and each names the other.
#
# ## Why a witness
#
# The never-accepting dead-peer leg holds the launcher's own `direct-ms` + `cache-ms` to a ceiling
# of (2N - 1/2) x TOTAL, 7000 ms at N = 2 and TOTAL = 2000. A loaded Windows runner reported 7602
# and then 8132 ms, and 8132 is past the 8000 a launcher that DOUBLED its deadline reports, so time
# alone cannot tell a starved host from a doubled deadline. Raising the ceiling would stop catching
# the doubled one. So a sibling thread ticks beside the launcher and records how long the HOST
# stalled it; an overrun the host stalled at least as long as is INCONCLUSIVE rather than a failure.
#
# ## Why STALLED time and not lateness
#
# Every tick asks for a `$WitnessTickMs` sleep and is late by the timer's granularity whatever the
# host does: summed, that figure is the TIMER, not the host, and it is larger than the 1000 ms a
# doubled deadline overruns the ceiling by -- so it would excuse exactly the regression the ceiling
# is there to catch (review of #1739, C1). Only a tick's EXCESS over `$WitnessStallMs` counts.
#
# MEASURED, 8 s a run, on an idle 32-thread Windows 11 host (about 5% CPU busy), 2026-10-29, three
# runs against an absent log and two against an existing one:
#
#   witness                                ticks     raw lateness   worst tick   stalled (> 50)
#   pwsh 7.6.7, this module, 5 runs        235-251   2924-3221 ms   25-31 ms     0 ms
#
# and the POSIX witness's own figures on the same host, under Git Bash's perl and WSL, are beside
# `e2e_witness_stall_ms`. A quiet host's stalled figure is far below TOTAL / 2 = 1000 ms, so a
# doubled deadline on a quiet host is a `fail`. The cost, failing CLOSED: a host that starves in
# slices under the threshold counts nothing, and every counted stall loses its first 50 ms.
#
# ## Its window, and its blind spots
#
# The witness ends when the launcher's stderr carries the line it prints once its last cache
# exchange failed, so its window is the CACHE phase rather than the compile after it. That window
# still FAILS OPEN at both ends, and is wider than the time judged: it opens before the launcher's
# process does, so a stall during the start counts; it spans the launcher's own work between its
# exchanges, which neither figure times; and it closes up to one tick plus however long it took to
# notice the line. A reading that never saw the line ran until it was stopped and may include the
# compile. And it fails OPEN where a host starves the witness and not the launcher. It fails
# CLOSED where a host starves in slices under the threshold, and where it starves only the
# launcher, the right way round.

Set-StrictMode -Version Latest

# The tick and the STALL threshold, in ms. The SAME numbers as `e2e_witness_tick_ms` and
# `e2e_witness_stall_ms` in `scripts/lib/e2e-common.sh`, which carry the reasoning. Here 50 is
# about 1.6x the worst quiet tick, 31 ms: `Thread.Sleep(20)` rounds up to two 15.6 ms timer
# periods, about 11 ms late, and a tick now and then waits a period more. Perl's worst was 15.
$script:WitnessTickMs = 20
$script:WitnessStallMs = 50

# How long a stop waits for the ticker to notice it before stopping its pipeline outright.
$script:WitnessStopBoundMs = 5000

# Start the witness on a runspace of its own: a THREAD of this process, scheduled by the host as
# the launcher is, which is what makes its stalls the host's. It ends by itself once @LogPath
# contains @Marker, or once @BoundMs of a monotonic clock have passed, whichever is first.
#
# @param LogPath the launcher's stderr, polled once a tick; opened sharing read, write and delete,
#                since the launcher still holds it.
# @param Marker the line in it that ends the cache phase.
# @param BoundMs the witness's lifetime bound.
# @return a handle for `Stop-E2EStarvationWitness`.
function Start-E2EStarvationWitness([string]$LogPath, [string]$Marker, [long]$BoundMs) {
    $sync = [hashtable]::Synchronized(@{
        Stop = $false; Ticks = 0; LateMs = 0.0; StalledMs = 0.0; WorstMs = 0.0; Closed = $false
    })
    $ps = [powershell]::Create()
    $null = $ps.AddScript({
        param($sync, [string]$log, [string]$marker, [int]$tickMs, [int]$stallMs, [long]$boundMs)
        $life = [System.Diagnostics.Stopwatch]::StartNew()
        $tick = [System.Diagnostics.Stopwatch]::new()
        while (-not $sync.Stop -and $life.ElapsedMilliseconds -lt $boundMs) {
            $tick.Restart()
            [System.Threading.Thread]::Sleep($tickMs)
            $over = $tick.Elapsed.TotalMilliseconds - $tickMs
            $sync.Ticks++
            if ($over -gt 0) {
                $sync.LateMs += $over
                if ($over -gt $stallMs) { $sync.StalledMs += $over - $stallMs }
                if ($over -gt $sync.WorstMs) { $sync.WorstMs = $over }
            }
            try {
                $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
                $stream = [System.IO.FileStream]::new($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
                try { $text = [System.IO.StreamReader]::new($stream).ReadToEnd() } finally { $stream.Dispose() }
                if ($text.Contains($marker)) { $sync.Closed = $true; break }
            } catch {
                # Not there yet, or briefly unreadable: the next tick asks again.
            }
        }
    }).AddArgument($sync).AddArgument($LogPath).AddArgument($Marker).AddArgument($script:WitnessTickMs).AddArgument($script:WitnessStallMs).AddArgument($BoundMs)
    return [pscustomobject]@{ PowerShell = $ps; Async = $ps.BeginInvoke(); Sync = $sync }
}

# Stop the witness and return its reading. Bounded: a ticker that does not notice the stop within
# `$WitnessStopBoundMs` has its pipeline stopped, and its reading says it did not END -- a reading
# then possibly taken mid-tick, which `Format-E2EStarvationWitness` names.
#
# @param Witness the handle `Start-E2EStarvationWitness` returned, or $null.
# @return @{ Ticks; LateMs; StalledMs; WorstMs (whole ms); Closed; Ended; TickMs; StallMs }, or
#         $null for a $null handle -- a witness that never started has no reading.
function Stop-E2EStarvationWitness($Witness) {
    if ($null -eq $Witness) { return $null }
    $Witness.Sync.Stop = $true
    $ended = $Witness.Async.AsyncWaitHandle.WaitOne($script:WitnessStopBoundMs)
    try {
        if ($ended) { $null = $Witness.PowerShell.EndInvoke($Witness.Async) }
        else { $Witness.PowerShell.Stop() }
    } catch {
        $ended = $false
    } finally {
        $Witness.PowerShell.Dispose()
    }
    $s = $Witness.Sync
    return [pscustomobject]@{
        Ticks     = [long]$s.Ticks
        LateMs    = [long][math]::Floor($s.LateMs)
        StalledMs = [long][math]::Floor($s.StalledMs)
        WorstMs   = [long][math]::Floor($s.WorstMs)
        Closed    = [bool]$s.Closed
        Ended     = [bool]$ended
        TickMs    = $script:WitnessTickMs
        StallMs   = $script:WitnessStallMs
    }
}

# A whole, non-negative number, as an integer or as its digits and nothing else. `[long]'1500 '`
# converts, which is why this is a pattern and not a cast: a reading with anything around its
# digits is not one. `\A` and `\z`, never `^` and `$`: `$` also matches before a final newline, so
# "1500`n" would pass.
function Test-WholeNumber($Value) {
    return ($null -ne $Value) -and ("$Value" -match '\A[0-9]+\z')
}

# Judge a measured time against its ceiling, given the witness's STALLED figure: `pass`, `fail` or
# `inconclusive`. Pure -- it reads no clock and starts nothing.
#
#   spent <= ceiling                       pass
#   over, witness unusable (ticks 0)       fail -- no evidence of starvation, so CLOSED
#   over, stalled >= spent - ceiling       inconclusive -- the overrun is not attributable
#   otherwise                              fail
#
# Never the raw lateness: that is the timer's granularity (see the head of this module). A
# malformed spent or ceiling is the CALLER's bug and THROWS rather than answering, and so is one with
# a LEADING ZERO: the bash twin's arithmetic reads `08132` as octal and refuses it, so this one
# refuses it too rather than the two helpers answering differently for one reading.
#
# @param SpentMs the launcher's reported time, ms.
# @param CeilingMs the ceiling, ms.
# @param Ticks the witness's tick count.
# @param StalledMs the witness's stalled time, ms.
# @return 'pass', 'fail' or 'inconclusive'.
function Get-E2EDeadCeilingVerdict($SpentMs, $CeilingMs, $Ticks, $StalledMs) {
    if (-not (Test-WholeNumber $SpentMs)) { throw "Get-E2EDeadCeilingVerdict: spent '$SpentMs' is not a whole number" }
    if (-not (Test-WholeNumber $CeilingMs)) { throw "Get-E2EDeadCeilingVerdict: ceiling '$CeilingMs' is not a whole number" }
    if ("$SpentMs" -match '\A0[0-9]') { throw "Get-E2EDeadCeilingVerdict: spent '$SpentMs' has a leading zero" }
    if ("$CeilingMs" -match '\A0[0-9]') { throw "Get-E2EDeadCeilingVerdict: ceiling '$CeilingMs' has a leading zero" }
    if ([long]"$SpentMs" -le [long]"$CeilingMs") { return 'pass' }
    # An unreadable witness is the absence of evidence, never evidence of a quiet host.
    if (-not (Test-WholeNumber $Ticks) -or -not (Test-WholeNumber $StalledMs)) { return 'fail' }
    if ([long]"$Ticks" -eq 0) { return 'fail' }
    if ([long]"$StalledMs" -ge ([long]"$SpentMs" - [long]"$CeilingMs")) { return 'inconclusive' }
    return 'fail'
}

# The reading as a sentence for a fixture's line: what it stalled, what it was late in all, and
# which window it covered -- or that there was no reading, which is not a quiet host.
#
# @param Reading what `Stop-E2EStarvationWitness` returned, or $null.
# @return the sentence.
function Format-E2EStarvationWitness($Reading) {
    if ($null -eq $Reading -or $Reading.Ticks -le 0) {
        return "the starvation witness left no reading, so it did not run and nothing can excuse the overrun -- that fails CLOSED"
    }
    $window = if ($Reading.Closed) { "the cache phase" } else { "until it was stopped, never having seen the fall-back line, so the compile too" }
    $ended = if ($Reading.Ended) { "" } else { "; it did not end when asked, so this reading may be mid-tick" }
    return ("starvation witness: {0} ms stalled past {1} ms a tick ({2} ms late in all, worst tick {3} ms) over {4} tick(s) of {5} ms, through {6}{7}" -f `
        $Reading.StalledMs, $Reading.StallMs, $Reading.LateMs, $Reading.WorstMs, $Reading.Ticks, $Reading.TickMs, $window, $ended)
}

# @return 0 when every case held, 1 otherwise -- each case named, and how many ran.
function Invoke-E2EStarvationWitnessSelfTest {
    $script:Cases = 0
    $script:Bad = 0
    function Expect([string]$what, $want, $got) {
        $script:Cases++
        if ("$want" -eq "$got") { Write-Host "  ok   $what" }
        else { Write-Host "  FAIL ${what}: want [$want] got [$got]"; $script:Bad++ }
    }
    function ExpectThrow([string]$what, [scriptblock]$body, [string]$pattern) {
        $script:Cases++
        try { $null = & $body } catch {
            if ($_.Exception.Message -match $pattern) { Write-Host "  ok   $what"; return }
            Write-Host "  FAIL ${what}: threw '$($_.Exception.Message)', want a message matching '$pattern'"; $script:Bad++; return
        }
        Write-Host "  FAIL ${what}: answered; want a throw matching '$pattern'"; $script:Bad++
    }

    Write-Host "starvation witness self-test"

    # The verdict, over synthesised readings. The two `as measured` rows are a doubled deadline
    # (8006-8017 ms) beside the stalled figure an idle host produced over 8 s: 0, so a fail. The
    # trailing space and the `x` are what get past a cast when a guard is gone, so they bite.
    Expect "within" 'pass' (Get-E2EDeadCeilingVerdict 7000 7000 10 0)
    Expect "within, no witness" 'pass' (Get-E2EDeadCeilingVerdict 6000 7000 0 0)
    Expect "starved" 'inconclusive' (Get-E2EDeadCeilingVerdict 8132 7000 50 1500)
    Expect "boundary: stalled == overrun" 'inconclusive' (Get-E2EDeadCeilingVerdict 8132 7000 50 1132)
    Expect "one under the boundary" 'fail' (Get-E2EDeadCeilingVerdict 8132 7000 50 1131)
    Expect "doubled on a quiet host" 'fail' (Get-E2EDeadCeilingVerdict 8010 7000 50 3)
    Expect "doubled, as measured under pwsh" 'fail' (Get-E2EDeadCeilingVerdict 8017 7000 250 0)
    Expect "witness never ticked" 'fail' (Get-E2EDeadCeilingVerdict 8132 7000 0 9999)
    Expect "witness unreadable" 'fail' (Get-E2EDeadCeilingVerdict 8132 7000 $null $null)
    Expect "ticks not a number" 'fail' (Get-E2EDeadCeilingVerdict 8132 7000 'x' 9999)
    Expect "stalled with a trailing space" 'fail' (Get-E2EDeadCeilingVerdict 8132 7000 50 '1500 ')
    Expect "stalled with a trailing newline" 'fail' (Get-E2EDeadCeilingVerdict 8132 7000 50 "1500`n")
    ExpectThrow "a malformed spent throws" { Get-E2EDeadCeilingVerdict 'abc' 7000 1 1 } "spent 'abc' is not a whole number"
    ExpectThrow "a malformed ceiling throws" { Get-E2EDeadCeilingVerdict 8132 '7000 ' 1 1 } "ceiling '7000 ' is not a whole number"
    ExpectThrow "a leading-zero spent throws" { Get-E2EDeadCeilingVerdict '08132' 7000 1 1 } "spent '08132' has a leading zero"
    ExpectThrow "a leading-zero ceiling throws" { Get-E2EDeadCeilingVerdict 8132 '07000' 1 1 } "ceiling '07000' has a leading zero"

    # The tick and the threshold are the bash twin's numbers, READ from `e2e-common.sh` rather than
    # remembered: two copies kept in step by hand drift without anything failing. Each assignment
    # must be found exactly once, or the comparison says nothing.
    $common = Get-Content -Raw (Join-Path $PSScriptRoot "e2e-common.sh")
    foreach ($pair in @(@('e2e_witness_tick_ms', $script:WitnessTickMs), @('e2e_witness_stall_ms', $script:WitnessStallMs))) {
        $found = @([regex]::Matches($common, "(?m)^$($pair[0])=([0-9]+)$"))
        Expect "e2e-common.sh assigns $($pair[0]) exactly once" 1 $found.Count
        $value = if ($found.Count -eq 1) { [int]$found[0].Groups[1].Value } else { $null }
        Expect "$($pair[0]) in e2e-common.sh is this module's $($pair[1])" $pair[1] $value
    }

    # The sentence: no reading is said as such, never as a quiet host.
    Expect "no reading is named" $true ((Format-E2EStarvationWitness $null).Contains("left no reading"))
    Expect "a reading names its window" $true `
        ((Format-E2EStarvationWitness ([pscustomobject]@{ Ticks = 3; LateMs = 30; StalledMs = 0; WorstMs = 11; Closed = $true; Ended = $true; TickMs = 20; StallMs = 50 })).Contains("through the cache phase"))

    # The witness itself. Its TIMING is not asserted -- a self-test shares the host with whatever
    # else is running -- only the part that does not depend on the clock: a log that already holds
    # the line closes it at its FIRST tick and it ends by itself, and one that never does keeps it
    # ticking until it is stopped, which says it did not close. "Keeps ticking" is waited for, not
    # slept for: up to `$tickBoundMs` for TWO ticks -- a tick past the first, which is the one a log
    # holding the line would have ended it at -- and a wait that runs out is its own named failure.
    $tickBoundMs = 10000
    function Wait-Ticks($witness, [int]$want) {
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        while ($witness.Sync.Ticks -lt $want -and -not $witness.Async.IsCompleted -and $clock.ElapsedMilliseconds -lt $tickBoundMs) {
            [System.Threading.Thread]::Sleep(10)
        }
        return $witness.Sync.Ticks -ge $want
    }
    $scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("pace-witness-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $scratch | Out-Null
    try {
        $closing = Join-Path $scratch "closing.log"
        Set-Content -Path $closing -Value "pace-cc: cache exchange (fetch) with x: timed out`npace-cc: warning (fetch exchange failed)"
        $w = Start-E2EStarvationWitness $closing '(fetch exchange failed)' 10000
        $finished = $w.Async.AsyncWaitHandle.WaitOne(5000)
        $reading = Stop-E2EStarvationWitness $w
        Expect "a log holding the line ends the witness by itself" $true $finished
        Expect "at its first tick" 1 $reading.Ticks
        Expect "and says it closed" $true $reading.Closed

        $open = Join-Path $scratch "open.log"
        Set-Content -Path $open -Value "pace-cc: cache exchange (fetch) with x: timed out"
        $w = Start-E2EStarvationWitness $open '(fetch exchange failed)' 60000
        $ticked = Wait-Ticks $w 2
        $early = $w.Async.IsCompleted
        $reading = Stop-E2EStarvationWitness $w
        Expect "a log without the line: two ticks within $tickBoundMs ms" $true $ticked
        Expect "a log without the line does not end it" $false $early
        Expect "it never closed" $false $reading.Closed
        Expect "and ended when asked" $true $reading.Ended

        # A log that does not exist yet is the launcher not having started: ticking, not closed.
        $w = Start-E2EStarvationWitness (Join-Path $scratch "absent.log") '(fetch exchange failed)' 60000
        $ticked = Wait-Ticks $w 2
        $early = $w.Async.IsCompleted
        $reading = Stop-E2EStarvationWitness $w
        Expect "an absent log: two ticks within $tickBoundMs ms" $true $ticked
        Expect "an absent log does not end it" $false $early
        Expect "an absent log keeps it open" $false $reading.Closed
    } finally {
        Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
    }

    Write-Host "starvation witness self-test: $($script:Cases) case(s) ran, $($script:Bad) failed"
    if ($script:Bad -gt 0) { return 1 }
    return 0
}

Export-ModuleMember -Function Start-E2EStarvationWitness, Stop-E2EStarvationWitness, Get-E2EDeadCeilingVerdict, `
    Format-E2EStarvationWitness, Invoke-E2EStarvationWitnessSelfTest
