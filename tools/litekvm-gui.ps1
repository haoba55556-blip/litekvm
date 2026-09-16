# LiteKVM GUI driver (UI Automation).
# ASCII-ONLY FILE ON PURPOSE: PowerShell 5.1 reads .ps1 as ANSI/GBK when there is
# no BOM, so any non-ASCII byte in this file corrupts parsing. CJK labels are
# therefore built from code points with -join (note: [char]0x542F + [char]0x52A8
# is INTEGER addition, not concatenation).
#   -Action list | status | click-start | click-stop | click-pair | set-server | set-client
param([string]$Action = "list")

Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes

function Esc([string]$s) {
  $o = ""
  foreach ($ch in $s.ToCharArray()) {
    $c = [int][char]$ch
    if ($c -gt 126 -or $c -lt 32) { $o += ("\u{0:X4}" -f $c) } else { $o += $ch }
  }
  return $o
}

# Qt widgets expose different patterns than Win32: try them all.
function Invoke-Element($e) {
  try { $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); return "InvokePattern" } catch {}
  try { $e.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select(); return "SelectionItemPattern" } catch {}
  try { $e.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle(); return "TogglePattern" } catch {}
  try { $e.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern).DoDefaultAction(); return "LegacyIAccessible" } catch {}
  return "NO_PATTERN_WORKED"
}

# label sets: [0] = zh (code points), [1] = en
$L_START  = @((-join @([char]0x542F, [char]0x52A8)), "Start",
              (-join @([char]0x8FDE, [char]0x63A5)), "Connect")
$L_STOP   = @((-join @([char]0x505C, [char]0x6B62)), "Stop")
$L_PAIR   = @((-join @([char]0x914D, [char]0x5BF9)), "Pair")
$L_SERVER = @((-join @([char]0x4F7F, [char]0x7528, [char]0x6B64)), "Use this")
$L_CLIENT = @((-join @([char]0x4F7F, [char]0x7528, [char]0x53E6)), "Use another")
# service-offline dialog buttons: ignore / retry / disable
$L_IGNORE  = @((-join @([char]0x5FFD, [char]0x7565)), "Ignore")
$L_RETRY   = @((-join @([char]0x91CD, [char]0x8BD5)), "Retry")
$L_DISABLE = @((-join @([char]0x7981, [char]0x7528)), "Disable")
# trust-confirmation dialog: yes / no
$L_YES = @((-join @([char]0x662F)), "Yes")
$L_NO  = @((-join @([char]0x5426)), "No")

$p = Get-Process deskflow -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output "NO_PROCESS"; exit 1 }

$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
$win = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $win) { Write-Output "NO_WINDOW"; exit 1 }

$all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)

function Click-ByName {
  param([string[]]$prefixes)
  Write-Output ("SEARCHING: " + ($prefixes -join ' | '))
  foreach ($e in $all) {
    $t = $e.Current.ControlType.ProgrammaticName
    if ($t -ne 'ControlType.Button' -and $t -ne 'ControlType.RadioButton') { continue }
    $n = $e.Current.Name
    if (-not $n) { continue }
    foreach ($pfx in $prefixes) {
      if ($pfx -and $n.StartsWith($pfx)) {
        $how = Invoke-Element $e
        Write-Output ("TARGET: " + (Esc $n) + " | via=" + $how)
        return
      }
    }
  }
  Write-Output "NO_MATCH"
}

switch ($Action) {
  "list" {
    Write-Output ("PID=" + $p.Id + " TITLE=" + (Esc $win.Current.Name))
    $i = 0
    foreach ($e in $all) {
      $t = $e.Current.ControlType.ProgrammaticName.Replace('ControlType.', '')
      $n = $e.Current.Name
      $sel = ""
      try { if ($e.GetCurrentPropertyValue([System.Windows.Automation.SelectionItemPattern]::IsSelectedProperty)) { $sel = " [SELECTED]" } } catch {}
      if ($n) { Write-Output ("$i | $t | " + (Esc $n) + $sel) }
      $i++
    }
  }
  "status" {
    foreach ($e in $all) {
      $t = $e.Current.ControlType.ProgrammaticName.Replace('ControlType.', '')
      $n = $e.Current.Name
      if ($n -and ($t -eq 'Text' -or $t -eq 'ListItem' -or $t -eq 'Edit')) { Write-Output ("$t | " + (Esc $n)) }
    }
  }
  "click-start" { Click-ByName -prefixes $L_START }
  "click-stop"  { Click-ByName -prefixes $L_STOP }
  "click-pair"  { Click-ByName -prefixes $L_PAIR }
  "set-server"  { Click-ByName -prefixes $L_SERVER }
  "set-client"  { Click-ByName -prefixes $L_CLIENT }
  "click-ignore" { Click-ByName -prefixes $L_IGNORE }
  "click-retry"  { Click-ByName -prefixes $L_RETRY }
  "click-disable" { Click-ByName -prefixes $L_DISABLE }
  "click-yes" { Click-ByName -prefixes $L_YES }
  "click-no"  { Click-ByName -prefixes $L_NO }
}
