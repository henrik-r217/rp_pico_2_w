<?php
declare(strict_types=1);

$config = require __DIR__ . '/config.php';

date_default_timezone_set($config['timezone'] ?? 'UTC');

function h($value): string {
    return htmlspecialchars((string)$value, ENT_QUOTES, 'UTF-8');
}

function getPdo(array $config): PDO {
    $dsn = sprintf(
        'mysql:host=%s;port=%d;dbname=%s;charset=%s',
        $config['host'],
        $config['port'],
        $config['dbname'],
        $config['charset'] ?? 'utf8mb4'
    );

    return new PDO($dsn, $config['user'], $config['pass'], [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        PDO::ATTR_EMULATE_PREPARES => false,
    ]);
}

function fetchDevices(PDO $pdo): array {
    $sql = "
        SELECT device_id
        FROM (
            SELECT DISTINCT device_id FROM batches WHERE device_id IS NOT NULL AND device_id <> ''
            UNION
            SELECT DISTINCT device_id FROM measurements WHERE device_id IS NOT NULL AND device_id <> ''
        ) t
        ORDER BY device_id ASC
    ";

    return $pdo->query($sql)->fetchAll(PDO::FETCH_COLUMN);
}

function fetchSummary(PDO $pdo, string $deviceId = ''): array {
    $where = '';
    $params = [];

    if ($deviceId !== '') {
        $where = 'WHERE device_id = :device_id';
        $params[':device_id'] = $deviceId;
    }

    $sql = "
        SELECT
            COUNT(*) AS measurements_total,
            MIN(device_timestamp_utc) AS min_ts,
            MAX(device_timestamp_utc) AS max_ts,
            AVG(temperature) AS avg_temperature,
            AVG(humidity) AS avg_humidity,
            MAX(seq) AS max_seq,
            MIN(seq) AS min_seq,
            SUM(CASE WHEN time_valid = 1 THEN 1 ELSE 0 END) AS time_valid_count
        FROM measurements
        $where
    ";

    $stmt = $pdo->prepare($sql);
    $stmt->execute($params);
    $row = $stmt->fetch() ?: [];

    $sql24 = "
        SELECT
            COUNT(*) AS measurements_24h,
            AVG(temperature) AS avg_temperature_24h,
            AVG(humidity) AS avg_humidity_24h
        FROM measurements
        WHERE server_received_at >= UNIX_TIMESTAMP(UTC_TIMESTAMP()) - 86400
    ";

    if ($deviceId !== '') {
        $sql24 .= ' AND device_id = :device_id';
    }

    $stmt24 = $pdo->prepare($sql24);
    $stmt24->execute($params);
    $row24 = $stmt24->fetch() ?: [];

    return [
        'measurements_total' => (int)($row['measurements_total'] ?? 0),
        'measurements_24h' => (int)($row24['measurements_24h'] ?? 0),
        'avg_temperature' => $row['avg_temperature'],
        'avg_humidity' => $row['avg_humidity'],
        'avg_temperature_24h' => $row24['avg_temperature_24h'],
        'avg_humidity_24h' => $row24['avg_humidity_24h'],
        'min_ts' => $row['min_ts'],
        'max_ts' => $row['max_ts'],
        'min_seq' => $row['min_seq'],
        'max_seq' => $row['max_seq'],
        'time_valid_count' => (int)($row['time_valid_count'] ?? 0),
    ];
}

function fetchLatestBatches(PDO $pdo, int $limit, string $deviceId = ''): array {
    $sql = "
        SELECT
            id,
            device_id,
            queued_total,
            dropped_total,
            time_valid,
            remote_ip,
            server_received_at,
            FROM_UNIXTIME(server_received_at) AS server_time,
            CHAR_LENGTH(raw_json) AS raw_json_length
        FROM batches
    ";

    $params = [];
    if ($deviceId !== '') {
        $sql .= ' WHERE device_id = :device_id';
        $params[':device_id'] = $deviceId;
    }

    $sql .= ' ORDER BY id DESC LIMIT :limit';
    $stmt = $pdo->prepare($sql);

    foreach ($params as $key => $value) {
        $stmt->bindValue($key, $value, PDO::PARAM_STR);
    }
    $stmt->bindValue(':limit', $limit, PDO::PARAM_INT);
    $stmt->execute();

    return $stmt->fetchAll();
}

function fetchLatestMeasurements(PDO $pdo, int $limit, string $deviceId = ''): array {
    $sql = "
        SELECT
            id,
            batch_id,
            device_id,
            seq,
            device_timestamp_utc,
            FROM_UNIXTIME(device_timestamp_utc) AS device_time,
            server_received_at,
            FROM_UNIXTIME(server_received_at) AS server_time,
            (server_received_at - device_timestamp_utc) AS delay_s,
            time_valid,
            uptime_s,
            temperature,
            humidity
        FROM measurements
    ";

    $params = [];
    if ($deviceId !== '') {
        $sql .= ' WHERE device_id = :device_id';
        $params[':device_id'] = $deviceId;
    }

    $sql .= ' ORDER BY id DESC LIMIT :limit';
    $stmt = $pdo->prepare($sql);

    foreach ($params as $key => $value) {
        $stmt->bindValue($key, $value, PDO::PARAM_STR);
    }
    $stmt->bindValue(':limit', $limit, PDO::PARAM_INT);
    $stmt->execute();

    return $stmt->fetchAll();
}

function fetchHourlyPoints(PDO $pdo, string $deviceId = ''): array {
    $sql = "
        SELECT
            DATE_FORMAT(FROM_UNIXTIME(server_received_at), '%Y-%m-%d %H:00') AS hour_bucket,
            COUNT(*) AS samples,
            AVG(temperature) AS avg_temperature,
            AVG(humidity) AS avg_humidity
        FROM measurements
        WHERE server_received_at >= UNIX_TIMESTAMP(UTC_TIMESTAMP()) - 86400
    ";

    $params = [];
    if ($deviceId !== '') {
        $sql .= ' AND device_id = :device_id';
        $params[':device_id'] = $deviceId;
    }

    $sql .= ' GROUP BY hour_bucket ORDER BY hour_bucket ASC';
    $stmt = $pdo->prepare($sql);
    $stmt->execute($params);
    return $stmt->fetchAll();
}

$selectedDevice = isset($_GET['device']) ? trim((string)$_GET['device']) : ($config['default_device'] ?? '');
$errorMessage = '';
$devices = [];
$summary = [];
$batches = [];
$measurements = [];
$hourly = [];

try {
    $pdo = getPdo($config);
    $devices = fetchDevices($pdo);

    if ($selectedDevice !== '' && !in_array($selectedDevice, $devices, true)) {
        $selectedDevice = '';
    }

    $summary = fetchSummary($pdo, $selectedDevice);
    $batches = fetchLatestBatches($pdo, (int)($config['page_size_batches'] ?? 20), $selectedDevice);
    $measurements = fetchLatestMeasurements($pdo, (int)($config['page_size_measurements'] ?? 100), $selectedDevice);
    $hourly = fetchHourlyPoints($pdo, $selectedDevice);
} catch (Throwable $e) {
    $errorMessage = $e->getMessage();
}
?>
<!doctype html>
<html lang="sv">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Pico Telemetry Dashboard</title>
    <style>
        :root {
            --bg: #0b1020;
            --panel: #121933;
            --panel-2: #182245;
            --text: #eef2ff;
            --muted: #aab4d6;
            --accent: #7aa2ff;
            --ok: #3dd68c;
            --bad: #ff6b6b;
            --line: rgba(255,255,255,0.08);
        }
        * { box-sizing: border-box; }
        body {
            margin: 0;
            font-family: Inter, system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif;
            background: linear-gradient(180deg, #0b1020 0%, #131b33 100%);
            color: var(--text);
        }
        .wrap {
            width: min(1400px, calc(100% - 24px));
            margin: 24px auto;
        }
        .topbar {
            display: flex;
            flex-wrap: wrap;
            gap: 16px;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 20px;
        }
        h1 {
            margin: 0;
            font-size: 28px;
            line-height: 1.2;
        }
        .sub {
            color: var(--muted);
            margin-top: 6px;
            font-size: 14px;
        }
        form.filter {
            display: flex;
            gap: 10px;
            align-items: center;
            background: var(--panel);
            border: 1px solid var(--line);
            border-radius: 14px;
            padding: 12px 14px;
        }
        select, button {
            border-radius: 10px;
            border: 1px solid var(--line);
            background: var(--panel-2);
            color: var(--text);
            padding: 10px 12px;
            font-size: 14px;
        }
        button {
            cursor: pointer;
            background: var(--accent);
            color: #081022;
            border: none;
            font-weight: 700;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            gap: 14px;
            margin-bottom: 20px;
        }
        .card {
            background: rgba(18, 25, 51, 0.88);
            border: 1px solid var(--line);
            border-radius: 18px;
            padding: 18px;
            box-shadow: 0 14px 40px rgba(0,0,0,0.2);
        }
        .label {
            color: var(--muted);
            font-size: 13px;
            margin-bottom: 10px;
        }
        .value {
            font-size: 28px;
            font-weight: 800;
            letter-spacing: 0.02em;
        }
        .small {
            margin-top: 8px;
            color: var(--muted);
            font-size: 13px;
        }
        .panel {
            background: rgba(18, 25, 51, 0.88);
            border: 1px solid var(--line);
            border-radius: 18px;
            padding: 18px;
            margin-bottom: 20px;
        }
        .panel h2 {
            margin: 0 0 14px;
            font-size: 20px;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            font-size: 14px;
        }
        th, td {
            padding: 10px 8px;
            border-bottom: 1px solid var(--line);
            vertical-align: top;
            text-align: left;
        }
        th {
            color: var(--muted);
            font-weight: 600;
        }
        tr:hover td { background: rgba(255,255,255,0.03); }
        .status-ok { color: var(--ok); font-weight: 700; }
        .status-bad { color: var(--bad); font-weight: 700; }
        .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
        .error {
            background: rgba(255, 107, 107, 0.12);
            border: 1px solid rgba(255,107,107,0.35);
            color: #ffd0d0;
            padding: 16px;
            border-radius: 14px;
        }
        .two-col {
            display: grid;
            grid-template-columns: 1.15fr 1fr;
            gap: 20px;
        }
        .mini-list {
            display: grid;
            gap: 8px;
        }
        .mini-item {
            display: flex;
            justify-content: space-between;
            gap: 16px;
            border-bottom: 1px dashed var(--line);
            padding-bottom: 8px;
        }
        .muted { color: var(--muted); }
        @media (max-width: 1100px) {
            .grid { grid-template-columns: repeat(2, 1fr); }
            .two-col { grid-template-columns: 1fr; }
        }
        @media (max-width: 700px) {
            .grid { grid-template-columns: 1fr; }
            table { display: block; overflow-x: auto; white-space: nowrap; }
            .topbar { align-items: stretch; }
            form.filter { width: 100%; justify-content: space-between; }
        }
    </style>
</head>
<body>
<div class="wrap">
    <div class="topbar">
        <div>
            <h1>Pico Telemetry Dashboard</h1>
            <div class="sub">Data från MySQL på webbhotellet • tabeller: <span class="mono">batches</span> och <span class="mono">measurements</span></div>
        </div>
        <form class="filter" method="get">
            <label for="device" class="muted">Enhet</label>
            <select name="device" id="device">
                <option value="">Alla enheter</option>
                <?php foreach ($devices as $device): ?>
                    <option value="<?= h($device) ?>" <?= $device === $selectedDevice ? 'selected' : '' ?>><?= h($device) ?></option>
                <?php endforeach; ?>
            </select>
            <button type="submit">Filtrera</button>
        </form>
    </div>

    <?php if ($errorMessage !== ''): ?>
        <div class="error">
            <strong>Databasfel:</strong><br>
            <?= h($errorMessage) ?><br><br>
            Kontrollera <span class="mono">config.php</span> och databasstrukturen.
        </div>
    <?php else: ?>

    <div class="grid">
        <div class="card">
            <div class="label">Antal mätningar totalt</div>
            <div class="value"><?= h($summary['measurements_total'] ?? 0) ?></div>
            <div class="small">Vald enhet: <?= $selectedDevice !== '' ? h($selectedDevice) : 'Alla' ?></div>
        </div>
        <div class="card">
            <div class="label">Antal mätningar senaste 24h</div>
            <div class="value"><?= h($summary['measurements_24h'] ?? 0) ?></div>
            <div class="small">Baserat på <span class="mono">server_received_at</span></div>
        </div>
        <div class="card">
            <div class="label">Medeltemperatur</div>
            <div class="value"><?= isset($summary['avg_temperature']) ? number_format((float)$summary['avg_temperature'], 2) . ' °C' : '–' ?></div>
            <div class="small">24h: <?= isset($summary['avg_temperature_24h']) ? number_format((float)$summary['avg_temperature_24h'], 2) . ' °C' : '–' ?></div>
        </div>
        <div class="card">
            <div class="label">Tid giltig</div>
            <div class="value"><?= h($summary['time_valid_count'] ?? 0) ?></div>
            <div class="small">Antal rader där <span class="mono">time_valid = 1</span></div>
        </div>
    </div>

    <div class="two-col">
        <div class="panel">
            <h2>Översikt</h2>
            <div class="mini-list">
                <div class="mini-item"><span class="muted">Första device-tid</span><span><?= !empty($summary['min_ts']) ? h(date('Y-m-d H:i:s', (int)$summary['min_ts'])) : '–' ?></span></div>
                <div class="mini-item"><span class="muted">Senaste device-tid</span><span><?= !empty($summary['max_ts']) ? h(date('Y-m-d H:i:s', (int)$summary['max_ts'])) : '–' ?></span></div>
                <div class="mini-item"><span class="muted">Lägsta seq</span><span><?= isset($summary['min_seq']) ? h($summary['min_seq']) : '–' ?></span></div>
                <div class="mini-item"><span class="muted">Högsta seq</span><span><?= isset($summary['max_seq']) ? h($summary['max_seq']) : '–' ?></span></div>
                <div class="mini-item"><span class="muted">Medelfukt</span><span><?= isset($summary['avg_humidity']) ? number_format((float)$summary['avg_humidity'], 2) . ' %' : '–' ?></span></div>
                <div class="mini-item"><span class="muted">24h medelfukt</span><span><?= isset($summary['avg_humidity_24h']) ? number_format((float)$summary['avg_humidity_24h'], 2) . ' %' : '–' ?></span></div>
            </div>
        </div>

        <div class="panel">
            <h2>Timvisa aggregat (senaste 24h)</h2>
            <table>
                <thead>
                    <tr>
                        <th>Timme</th>
                        <th>Samples</th>
                        <th>Temp</th>
                        <th>Hum</th>
                    </tr>
                </thead>
                <tbody>
                <?php if (!$hourly): ?>
                    <tr><td colspan="4" class="muted">Ingen data för senaste 24h.</td></tr>
                <?php else: ?>
                    <?php foreach ($hourly as $row): ?>
                        <tr>
                            <td><?= h($row['hour_bucket']) ?></td>
                            <td><?= h($row['samples']) ?></td>
                            <td><?= $row['avg_temperature'] !== null ? h(number_format((float)$row['avg_temperature'], 2)) : '–' ?></td>
                            <td><?= $row['avg_humidity'] !== null ? h(number_format((float)$row['avg_humidity'], 2)) : '–' ?></td>
                        </tr>
                    <?php endforeach; ?>
                <?php endif; ?>
                </tbody>
            </table>
        </div>
    </div>

    <div class="panel">
        <h2>Senaste batchar</h2>
        <table>
            <thead>
            <tr>
                <th>ID</th>
                <th>Enhet</th>
                <th>Server-tid</th>
                <th>Queue</th>
                <th>Dropped</th>
                <th>Time valid</th>
                <th>IP</th>
                <th>JSON bytes</th>
            </tr>
            </thead>
            <tbody>
            <?php if (!$batches): ?>
                <tr><td colspan="8" class="muted">Ingen batch-data hittades.</td></tr>
            <?php else: ?>
                <?php foreach ($batches as $row): ?>
                    <tr>
                        <td><?= h($row['id']) ?></td>
                        <td><?= h($row['device_id']) ?></td>
                        <td><?= h($row['server_time']) ?></td>
                        <td><?= h($row['queued_total']) ?></td>
                        <td><?= h($row['dropped_total']) ?></td>
                        <td class="<?= (int)$row['time_valid'] === 1 ? 'status-ok' : 'status-bad' ?>">
                            <?= (int)$row['time_valid'] === 1 ? 'Ja' : 'Nej' ?>
                        </td>
                        <td><?= h($row['remote_ip']) ?></td>
                        <td><?= h($row['raw_json_length']) ?></td>
                    </tr>
                <?php endforeach; ?>
            <?php endif; ?>
            </tbody>
        </table>
    </div>

    <div class="panel">
        <h2>Senaste mätningar</h2>
        <table>
            <thead>
            <tr>
                <th>ID</th>
                <th>Enhet</th>
                <th>Seq</th>
                <th>Device-tid</th>
                <th>Server-tid</th>
                <th>Delay (s)</th>
                <th>Temp</th>
                <th>Hum</th>
                <th>Uptime</th>
                <th>Time valid</th>
            </tr>
            </thead>
            <tbody>
            <?php if (!$measurements): ?>
                <tr><td colspan="10" class="muted">Ingen mätdata hittades.</td></tr>
            <?php else: ?>
                <?php foreach ($measurements as $row): ?>
                    <tr>
                        <td><?= h($row['id']) ?></td>
                        <td><?= h($row['device_id']) ?></td>
                        <td><?= h($row['seq']) ?></td>
                        <td><?= h($row['device_time']) ?></td>
                        <td><?= h($row['server_time']) ?></td>
                        <td><?= h($row['delay_s']) ?></td>
                        <td><?= $row['temperature'] !== null ? h(number_format((float)$row['temperature'], 2)) : '–' ?></td>
                        <td><?= $row['humidity'] !== null ? h(number_format((float)$row['humidity'], 2)) : '–' ?></td>
                        <td><?= h($row['uptime_s']) ?></td>
                        <td class="<?= (int)$row['time_valid'] === 1 ? 'status-ok' : 'status-bad' ?>">
                            <?= (int)$row['time_valid'] === 1 ? 'Ja' : 'Nej' ?>
                        </td>
                    </tr>
                <?php endforeach; ?>
            <?php endif; ?>
            </tbody>
        </table>
    </div>

    <?php endif; ?>
</div>
</body>
</html>
