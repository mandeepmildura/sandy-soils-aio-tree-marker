# AiO -> HiveMQ Cloud relay (Win PowerShell 5.1, no modules).
# Reads http://<aio>/status once a second, publishes JSON to
# treemarker/aio/status (retained). When hits or fires rises, publishes
# a one-shot message to treemarker/aio/mark with the latest lhi/lhm.
#
# Minimal MQTT 3.1.1 client (CONNECT / PUBLISH QoS 0 / PINGREQ) over
# System.Net.Security.SslStream. One AiO request at a time, no keep-alive,
# no idle sockets - the board hangs on parallel or long-lived sockets.
#
# Credentials go through Read-Host -AsSecureString + ConvertFrom-SecureString
# (DPAPI, per-user per-machine) in %APPDATA%\sandy-soils-aio\relay.xml.
# Password is never printed, never logged.

param(
  [string]$AioUrl = "http://192.168.5.126/status",
  [string]$TopicStatus = "treemarker/aio/status",
  [string]$TopicMark   = "treemarker/aio/mark",
  [switch]$Reconfigure,
  [string]$ConfigFile,
  [string]$MqttHost,
  [int]   $MqttPort = 8883,
  [switch]$NoTls,
  [string]$MqttUser,
  [string]$MqttPass,
  [int]   $PollMs = 1000,
  [int]   $AioTimeoutSec = 2,
  [int]   $KeepAliveSec = 30,
  [int]   $StatusEverySec = 10
)

$ErrorActionPreference = 'Stop'

# Write-Host is buffered when stdout is a pipe (tests, logfiles). Go
# straight to .NET's stdout so each line flushes on the way out.
function Say { param([string]$Msg) [Console]::Out.WriteLine($Msg); [Console]::Out.Flush() }

# ----- credential store ----------------------------------------------------
function Get-ConfigPath {
  if ($ConfigFile) { return $ConfigFile }
  $dir = Join-Path $env:APPDATA 'sandy-soils-aio'
  if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
  Join-Path $dir 'relay.xml'
}

function Load-Config {
  param([string]$Path)
  if (-not (Test-Path $Path)) { return $null }
  try {
    $raw = Import-Clixml $Path
    $sec = ConvertTo-SecureString $raw.Pass
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try   { $plain = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    return @{ Host=$raw.Host; Port=[int]$raw.Port; Tls=[bool]$raw.Tls; User=$raw.User; Pass=$plain }
  } catch {
    Say "Saved config at $Path was unreadable: $($_.Exception.Message)"
    return $null
  }
}

function Prompt-Config {
  param([string]$Path)
  Say ''
  Say 'AiO -> HiveMQ relay: first-run setup'
  Say '------------------------------------'
  $h = Read-Host 'HiveMQ host (e.g. xxxxx.s2.eu.hivemq.cloud)'
  $portStr = Read-Host 'Port [8883]'
  $port = if ([string]::IsNullOrWhiteSpace($portStr)) { 8883 } else { [int]$portStr }
  $u = Read-Host 'Username (Surface publish user)'
  $sec = Read-Host 'Password' -AsSecureString
  $enc = ConvertFrom-SecureString $sec
  @{ Host=$h; Port=$port; Tls=$true; User=$u; Pass=$enc } | Export-Clixml $Path
  Say "Saved to $Path."
  $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
  try   { $plain = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
  finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
  return @{ Host=$h; Port=$port; Tls=$true; User=$u; Pass=$plain }
}

# ----- MQTT encoding -------------------------------------------------------
function Encode-MqttVarInt {
  param([int]$len)
  $out = New-Object System.Collections.Generic.List[byte]
  do {
    $b = $len -band 0x7F
    $len = $len -shr 7
    if ($len -gt 0) { $b = $b -bor 0x80 }
    $out.Add([byte]$b)
  } while ($len -gt 0)
  ,$out.ToArray()
}

function Encode-MqttString {
  param([string]$s)
  $bytes = [Text.Encoding]::UTF8.GetBytes($s)
  $out = [byte[]]::new(2 + $bytes.Length)
  $out[0] = [byte](($bytes.Length -shr 8) -band 0xFF)
  $out[1] = [byte]($bytes.Length -band 0xFF)
  [Array]::Copy($bytes, 0, $out, 2, $bytes.Length)
  ,$out
}

function Build-MqttConnect {
  param([string]$ClientId, [string]$User, [string]$Pass, [int]$KeepAlive)
  $ms = New-Object System.IO.MemoryStream
  $pn = [byte[]](0x00,0x04,0x4D,0x51,0x54,0x54); $ms.Write($pn, 0, 6)
  $ms.WriteByte(0x04)
  $flags = 0x02
  if ($User) { $flags = $flags -bor 0x80 }
  if ($Pass) { $flags = $flags -bor 0x40 }
  $ms.WriteByte([byte]$flags)
  $ms.WriteByte([byte](($KeepAlive -shr 8) -band 0xFF))
  $ms.WriteByte([byte]($KeepAlive -band 0xFF))
  $cid = Encode-MqttString $ClientId; $ms.Write($cid, 0, $cid.Length)
  if ($User) { $u = Encode-MqttString $User; $ms.Write($u, 0, $u.Length) }
  if ($Pass) { $p = Encode-MqttString $Pass; $ms.Write($p, 0, $p.Length) }
  $body = $ms.ToArray()
  $rl = Encode-MqttVarInt $body.Length
  $pkt = [byte[]]::new(1 + $rl.Length + $body.Length)
  $pkt[0] = 0x10
  [Array]::Copy($rl, 0, $pkt, 1, $rl.Length)
  [Array]::Copy($body, 0, $pkt, 1 + $rl.Length, $body.Length)
  ,$pkt
}

function Build-MqttPublish {
  param([string]$Topic, [string]$Payload, [bool]$Retain)
  $t = Encode-MqttString $Topic
  $p = [Text.Encoding]::UTF8.GetBytes($Payload)
  $bodyLen = $t.Length + $p.Length
  $rl = Encode-MqttVarInt $bodyLen
  $hdr = if ($Retain) { [byte]0x31 } else { [byte]0x30 }
  $pkt = [byte[]]::new(1 + $rl.Length + $bodyLen)
  $pkt[0] = $hdr
  [Array]::Copy($rl, 0, $pkt, 1, $rl.Length)
  [Array]::Copy($t, 0, $pkt, 1 + $rl.Length, $t.Length)
  [Array]::Copy($p, 0, $pkt, 1 + $rl.Length + $t.Length, $p.Length)
  ,$pkt
}

$script:MqttPing = [byte[]](0xC0, 0x00)

# ----- MQTT connection -----------------------------------------------------
function Start-MqttConnect {
  # Returns a pending-connect object: { Tcp, Task, Cfg }. The main loop
  # polls Finish-MqttConnect each tick so reconnect never blocks AiO polling
  # (a Connect to a closed port on Windows can wait seconds for SYN retries).
  param([hashtable]$Cfg)
  $tcp = New-Object System.Net.Sockets.TcpClient
  $tcp.ReceiveTimeout = 10000
  $tcp.SendTimeout = 10000
  $task = $tcp.ConnectAsync($Cfg.Host, $Cfg.Port)
  [PSCustomObject]@{ Tcp=$tcp; Task=$task; Cfg=$Cfg; StartedMs=[Environment]::TickCount }
}

function Finish-MqttConnect {
  # Non-blocking poll. Returns:
  #  $null      -> still pending, try again next tick
  #  hashtable  -> { Status='ok'; Conn=... } or { Status='failed'; Reason=... }
  param($Pending, [int]$MaxWaitMs = 15000)
  if ($Pending.Task.IsCompleted) {
    $tcp = $Pending.Tcp
    if ($Pending.Task.IsFaulted -or $Pending.Task.IsCanceled) {
      $msg = if ($Pending.Task.Exception) { $Pending.Task.Exception.InnerException.Message } else { 'canceled' }
      try { $tcp.Close() } catch {}
      return @{ Status='failed'; Reason=$msg }
    }
    try {
      $tcp.NoDelay = $true
      $raw = $tcp.GetStream()
      $cfg = $Pending.Cfg
      if ($cfg.Tls) {
        $ssl = New-Object System.Net.Security.SslStream($raw, $false)
        $ssl.AuthenticateAsClient($cfg.Host, $null, [System.Security.Authentication.SslProtocols]::Tls12, $true)
        $stream = $ssl
      } else {
        $stream = $raw
      }
      $cid = 'aio-relay-' + [Guid]::NewGuid().ToString('N').Substring(0, 10)
      $pkt = Build-MqttConnect -ClientId $cid -User $cfg.User -Pass $cfg.Pass -KeepAlive $KeepAliveSec
      $stream.Write($pkt, 0, $pkt.Length); $stream.Flush()
      $buf = [byte[]]::new(4); $n = $stream.Read($buf, 0, 4)
      if ($n -lt 4 -or $buf[0] -ne 0x20) { throw "CONNACK header wrong (got $n bytes)" }
      if ($buf[3] -ne 0) { throw "CONNACK rc=$($buf[3])" }
      return @{ Status='ok'; Conn=[PSCustomObject]@{ Tcp=$tcp; Stream=$stream; LastSentMs=[Environment]::TickCount } }
    } catch {
      try { $tcp.Close() } catch {}
      return @{ Status='failed'; Reason=$_.Exception.Message }
    }
  }
  if (([Environment]::TickCount - $Pending.StartedMs) -gt $MaxWaitMs) {
    try { $Pending.Tcp.Close() } catch {}
    return @{ Status='failed'; Reason="connect timed out after $MaxWaitMs ms" }
  }
  $null
}

function Close-MqttConnection {
  param($Conn)
  if ($Conn) {
    try { $Conn.Stream.Dispose() } catch {}
    try { $Conn.Tcp.Close() } catch {}
  }
}

function Send-MqttPublish {
  param($Conn, [string]$Topic, [string]$Payload, [bool]$Retain)
  $pkt = Build-MqttPublish -Topic $Topic -Payload $Payload -Retain $Retain
  $Conn.Stream.Write($pkt, 0, $pkt.Length); $Conn.Stream.Flush()
  $Conn.LastSentMs = [Environment]::TickCount
}

function Send-MqttPing {
  param($Conn)
  $Conn.Stream.Write($script:MqttPing, 0, $script:MqttPing.Length); $Conn.Stream.Flush()
  $Conn.LastSentMs = [Environment]::TickCount
}

# ----- AiO fetch (one request at a time, no reuse) -------------------------
function Get-AioStatus {
  param([string]$Url, [int]$TimeoutSec)
  # Fresh WebRequest with KeepAlive=false guarantees no idle socket is left
  # around. ServicePoint.ConnectionLimit=1 keeps us from ever doubling up.
  $req = [System.Net.HttpWebRequest]::Create($Url)
  $req.KeepAlive = $false
  $req.Timeout = $TimeoutSec * 1000
  $req.ReadWriteTimeout = $TimeoutSec * 1000
  $req.Method = 'GET'
  $req.ServicePoint.ConnectionLimit = 1
  $req.ServicePoint.Expect100Continue = $false
  try {
    $resp = $req.GetResponse()
    try {
      $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
      try   { $body = $sr.ReadToEnd() }
      finally { $sr.Close() }
    } finally { $resp.Close() }
    try { $req.ServicePoint.CloseConnectionGroup([String]::Empty) | Out-Null } catch {}
    return $body
  } catch {
    try { $req.Abort() } catch {}
    throw
  }
}

# ----- main ---------------------------------------------------------------
$cfgPath = Get-ConfigPath

# Allow fully-headless launch via parameters (used by tests and anyone who
# wants to script it). Otherwise we load the saved config or prompt once.
$cfg = $null
if ($MqttHost) {
  $cfg = @{ Host=$MqttHost; Port=$MqttPort; Tls=(-not $NoTls); User=$MqttUser; Pass=$MqttPass }
} elseif ($Reconfigure) {
  $cfg = Prompt-Config -Path $cfgPath
} else {
  $cfg = Load-Config -Path $cfgPath
  if (-not $cfg) { $cfg = Prompt-Config -Path $cfgPath }
}

Say ""
Say "AiO -> HiveMQ relay"
Say "   AiO:     $AioUrl"
Say "   broker:  $($cfg.Host):$($cfg.Port) ($(if ($cfg.Tls) { 'TLS' } else { 'plain' })) as $($cfg.User)"
Say "   status:  $TopicStatus (retained, 1/s)"
Say "   mark:    $TopicMark (on hits/fires change)"
Say "   Ctrl+C to stop."
Say ""

$conn = $null
$pending = $null
$backoffMs = 1000
$nextReconnectMs = [Environment]::TickCount
$lastHits = $null
$lastFires = $null
$aioState = 'unknown'
$lastMark = '-'
$lastStatusPrintMs = 0
$nextTickMs = [Environment]::TickCount

while ($true) {
  $tickStart = [Environment]::TickCount

  # Start a new reconnect attempt only when idle and the backoff has elapsed.
  if (-not $conn -and -not $pending -and $tickStart -ge $nextReconnectMs) {
    try {
      $pending = Start-MqttConnect -Cfg $cfg
    } catch {
      $nextReconnectMs = [Environment]::TickCount + $backoffMs
      Say ("[" + (Get-Date -Format 'HH:mm:ss') + "] MQTT: connect start failed - " + $_.Exception.Message)
      if ($backoffMs -lt 30000) { $backoffMs = [Math]::Min(30000, $backoffMs * 2) }
    }
  }

  # Poll the in-flight connect without blocking.
  if ($pending) {
    $r = Finish-MqttConnect -Pending $pending
    if ($r) {
      if ($r.Status -eq 'ok') {
        $conn = $r.Conn
        $backoffMs = 1000
        Say ("[" + (Get-Date -Format 'HH:mm:ss') + "] MQTT: connected to " + $cfg.Host + ":" + $cfg.Port)
      } else {
        $nextReconnectMs = [Environment]::TickCount + $backoffMs
        Say ("[" + (Get-Date -Format 'HH:mm:ss') + "] MQTT: connect failed - " + $r.Reason + "; retry in " + [int]($backoffMs / 1000) + "s (AiO polling continues)")
        if ($backoffMs -lt 30000) { $backoffMs = [Math]::Min(30000, $backoffMs * 2) }
      }
      $pending = $null
    }
  }

  # One AiO fetch at a time.
  $body = $null
  try {
    $body = Get-AioStatus -Url $AioUrl -TimeoutSec $AioTimeoutSec
    if ($aioState -ne 'ok') {
      Say ("[{0}] AiO: back" -f (Get-Date -Format 'HH:mm:ss'))
      $aioState = 'ok'
    }
  } catch {
    if ($aioState -ne 'down') {
      Say ("[{0}] AiO: no answer ({1})" -f (Get-Date -Format 'HH:mm:ss'), $_.Exception.Message)
      $aioState = 'down'
    }
  }

  if ($body -and $conn) {
    try {
      Send-MqttPublish -Conn $conn -Topic $TopicStatus -Payload $body -Retain $true
    } catch {
      Say ("[" + (Get-Date -Format 'HH:mm:ss') + "] MQTT: publish failed - " + $_.Exception.Message + "; will reconnect")
      Close-MqttConnection $conn; $conn = $null
      $nextReconnectMs = [Environment]::TickCount + $backoffMs
    }

    # Mark event on hits/fires rising edge.
    if ($conn) {
      try {
        $j = $body | ConvertFrom-Json
        $hits  = [int64]$j.hits
        $fires = [int64]$j.fires
        $rose = $false
        if ($lastHits  -ne $null -and $hits  -gt $lastHits)  { $rose = $true }
        if ($lastFires -ne $null -and $fires -gt $lastFires) { $rose = $true }
        $lastHits  = $hits
        $lastFires = $fires
        if ($rose) {
          $mark = @{
            t     = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ss.fffzzz')
            hits  = $hits
            fires = $fires
            lhi   = $j.lhi
            lhm   = $j.lhm
            lat   = $j.la
            lon   = $j.lo
            spd   = $j.spd
            field = $j.field
          } | ConvertTo-Json -Compress
          Send-MqttPublish -Conn $conn -Topic $TopicMark -Payload $mark -Retain $false
          $lastMark = "lhi=$($j.lhi) lhm=$($j.lhm) (hits=$hits fires=$fires)"
          Say ("[{0}] MARK -> {1}" -f (Get-Date -Format 'HH:mm:ss'), $lastMark)
        }
      } catch {
        # Parsing a bad /status body or publishing the mark failed - log and carry on.
        Say ("[{0}] mark handling: {1}" -f (Get-Date -Format 'HH:mm:ss'), $_.Exception.Message)
      }
    }
  }

  # PINGREQ if the broker will time us out otherwise.
  if ($conn -and (([Environment]::TickCount - $conn.LastSentMs) -gt (($KeepAliveSec - 5) * 1000))) {
    try { Send-MqttPing -Conn $conn } catch {
      Say ("[{0}] MQTT: ping failed - {1}; will reconnect" -f (Get-Date -Format 'HH:mm:ss'), $_.Exception.Message)
      Close-MqttConnection $conn; $conn = $null
      $nextReconnectMs = [Environment]::TickCount + $backoffMs
    }
  }

  # One-liner summary every N seconds.
  if ((([Environment]::TickCount - $lastStatusPrintMs) -ge ($StatusEverySec * 1000))) {
    $mqttStateStr = if ($conn) { 'ok' } else { 'retrying' }
    Say ("[" + (Get-Date -Format 'HH:mm:ss') + "] AiO=" + $aioState + "  MQTT=" + $mqttStateStr + "  last mark: " + $lastMark)
    $lastStatusPrintMs = [Environment]::TickCount
  }

  # Schedule next tick so we publish ~1/s regardless of fetch cost.
  $nextTickMs += $PollMs
  $sleepMs = $nextTickMs - [Environment]::TickCount
  if ($sleepMs -lt 20) { $nextTickMs = [Environment]::TickCount; $sleepMs = 20 }
  Start-Sleep -Milliseconds $sleepMs
}
