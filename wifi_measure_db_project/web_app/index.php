<?php

$config = require __DIR__ . '/config.php';

function h($value): string {
    return htmlspecialchars((string)$value, ENT_QUOTES, 'UTF-8');
}

function getPdo(array $config): PDO {
    $dsn = sprintf(
        'mysql:host=%s;port=%d;dbname=%s;charset=%s',
        $config['host'],
        $config['port'] ?? 3306,
        $config['dbname'],
        $config['charset'] ?? 'utf8mb4'
    );

    return new PDO($dsn, $config['user'], $config['pass'], [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        PDO::ATTR_EMULATE_PREPARES => false,
    ]);
}

function hq(array $params): string {
    foreach ($params as $key => $value) {
        if ($value === null || $value === '' || $value === []) {
            unset($params[$key]);
        }
    }
    return http_build_query($params);
}

function selectedSeriesFromGet(): array {
    $allowed = ['temperature', 'humidity', 'delay'];

    if (!isset($_GET['series']) || !is_array($_GET['series'])) {
        return $allowed;
    }

    $selected = array_values(array_intersect($allowed, array_map('strval', $_GET['series'])));
    return $selected ?: $allowed;
}

function selectedTimeRange(): string {
    $allowed = ['24h', '7d', '30d', 'all'];
    $selected = isset($_GET['range']) ? (string)$_GET['range'] : '24h';
    return in_array($selected, $allowed, true) ? $selected : '24h';
}

function selectedResolution(): string {
    $allowed = ['hour', '10min'];
    $selected = isset($_GET['res']) ? (string)$_GET['res'] : 'hour';
    return in_array($selected, $allowed, true) ? $selected : 'hour';
}

function timeRangeSeconds(string $range): int {
    return match ($range) {
        '24h' => 86400,
        '7d' => 7 * 86400,
        '30d' => 30 * 86400,
        'all' => 0,
        default => 86400,
    };
}

function timeRangeLabel(string $range): string {
    return match ($range) {
        '24h' => 'senaste 24 timmarna',
        '7d' => 'senaste 7 dagarna',
        '30d' => 'senaste 30 dagarna',
        'all' => 'all tillgänglig data',
        default => 'vald period',
    };
}

function resolutionLabel(string $resolution): string {
    return match ($resolution) {
        '10min' => '10-minuters medel',
        'hour' => 'timvisa medel',
        default => 'timvisa medel',
    };
}

function buildWhereClause(string $deviceId, string $timeColumn, string $range): array {
    $clauses = [];
    $params = [];

    if ($deviceId !== '') {
        $clauses[] = 'device_id = :device_id';
        $params[':device_id'] = $deviceId;
    }

    if ($range !== 'all') {
        $clauses[] = sprintf('%s >= UNIX_TIMESTAMP(UTC_TIMESTAMP()) - :range_seconds', $timeColumn);
        $params[':range_seconds'] = timeRangeSeconds($range);
    }

    $sql = $clauses ? (' WHERE ' . implode(' AND ', $clauses) . ' ') : ' ';
    return [$sql, $params];
}

function bindParams(PDOStatement $stmt, array $params): void {
    foreach ($params as $key => $value) {
        $type = is_int($value) ? PDO::PARAM_INT : PDO::PARAM_STR;
        $stmt->bindValue($key, $value, $type);
    }
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

function fetchSummary(PDO $pdo, string $deviceId, string $range): array {
    [$whereMeasurements, $paramsMeasurements] = buildWhereClause($deviceId, 'server_received_at', $range);

    $sql = "
        SELECT
            COUNT(*) AS measurements_total,
            MIN(CASE WHEN time_valid = 1 THEN device_timestamp_utc END) AS min_ts,
            MAX(CASE WHEN time_valid = 1 THEN device_timestamp_utc END) AS max_ts,
            AVG(temperature) AS avg_temperature,
            AVG(humidity) AS avg_humidity,
            MAX(seq) AS max_seq,
            MIN(seq) AS min_seq,
            SUM(CASE WHEN time_valid = 1 THEN 1 ELSE 0 END) AS time_valid_count,
            AVG(CASE WHEN time_valid = 1 THEN (server_received_at - device_timestamp_utc) END) AS avg_delay_s,
            MAX(CASE WHEN time_valid = 1 THEN (server_received_at - device_timestamp_utc) END) AS max_delay_s
        FROM measurements
        $whereMeasurements
    ";

    $stmt = $pdo->prepare($sql);
    bindParams($stmt, $paramsMeasurements);
    $stmt->execute();
    $row = $stmt->fetch() ?: [];

    [$whereBatches, $paramsBatches] = buildWhereClause($deviceId, 'server_received_at', $range);

    $sqlBatch = "
        SELECT
            id,
            server_received_at,
            device_id,
            queued_total,
            dropped_total,
            time_valid,
            remote_ip
        FROM batches
        $whereBatches
        ORDER BY id DESC
        LIMIT 1
    ";

    $stmtBatch = $pdo->prepare($sqlBatch);
    bindParams($stmtBatch, $paramsBatches);
    $stmtBatch->execute();
    $latestBatch = $stmtBatch->fetch() ?: [];

    $sqlBatchAgg = "
        SELECT
            MAX(queued_total) AS max_queue,
            MIN(dropped_total) AS min_dropped,
            MAX(dropped_total) AS max_dropped,
            COUNT(*) AS batch_count
        FROM batches
        $whereBatches
    ";

    $stmtBatchAgg = $pdo->prepare($sqlBatchAgg);
    bindParams($stmtBatchAgg, $paramsBatches);
    $stmtBatchAgg->execute();
    $batchAgg = $stmtBatchAgg->fetch() ?: [];

    $droppedDelta = 0;
    if (($batchAgg['min_dropped'] ?? null) !== null && ($batchAgg['max_dropped'] ?? null) !== null) {
        $droppedDelta = (int)$batchAgg['max_dropped'] - (int)$batchAgg['min_dropped'];
    }

    $measurementsTotal = (int)($row['measurements_total'] ?? 0);
    $timeValidCount = (int)($row['time_valid_count'] ?? 0);
    $timeValidityRatio = $measurementsTotal > 0 ? ($timeValidCount / $measurementsTotal) * 100.0 : 0.0;

    $health = 'Ok';
    $healthLevel = 'ok';

    if ($measurementsTotal === 0) {
        $health = 'Ingen data i vald period';
        $healthLevel = 'warn';
    } elseif (((int)($latestBatch['time_valid'] ?? 1) === 0) || $droppedDelta > 0 || (float)($row['avg_delay_s'] ?? 0) > 300) {
        $health = 'Behöver uppmärksamhet';
        $healthLevel = 'bad';
    } elseif ((float)($row['avg_delay_s'] ?? 0) > 60 || (int)($batchAgg['max_queue'] ?? 0) > 20) {
        $health = 'Stabil men följ upp';
        $healthLevel = 'warn';
    }

    return [
        'measurements_total' => $measurementsTotal,
        'avg_temperature' => $row['avg_temperature'] ?? null,
        'avg_humidity' => $row['avg_humidity'] ?? null,
        'min_ts' => $row['min_ts'] ?? null,
        'max_ts' => $row['max_ts'] ?? null,
        'min_seq' => $row['min_seq'] ?? null,
        'max_seq' => $row['max_seq'] ?? null,
        'time_valid_count' => $timeValidCount,
        'time_valid_ratio' => $timeValidityRatio,
        'avg_delay_s' => $row['avg_delay_s'] ?? null,
        'max_delay_s' => $row['max_delay_s'] ?? null,
        'latest_batch' => $latestBatch,
        'max_queue' => (int)($batchAgg['max_queue'] ?? 0),
        'dropped_delta' => $droppedDelta,
        'latest_dropped_total' => isset($latestBatch['dropped_total']) ? (int)$latestBatch['dropped_total'] : 0,
        'batch_count' => (int)($batchAgg['batch_count'] ?? 0),
        'health' => $health,
        'health_level' => $healthLevel,
    ];
}

function fetchLatestBatches(PDO $pdo, int $limit, int $offset, string $deviceId, string $range): array {
    [$where, $params] = buildWhereClause($deviceId, 'server_received_at', $range);

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
        $where
        ORDER BY id DESC
        LIMIT :limit OFFSET :offset
    ";

    $stmt = $pdo->prepare($sql);
    bindParams($stmt, $params);
    $stmt->bindValue(':limit', $limit, PDO::PARAM_INT);
    $stmt->bindValue(':offset', $offset, PDO::PARAM_INT);
    $stmt->execute();

    return $stmt->fetchAll();
}

function countBatches(PDO $pdo, string $deviceId, string $range): int {
    [$where, $params] = buildWhereClause($deviceId, 'server_received_at', $range);
    $stmt = $pdo->prepare("SELECT COUNT(*) FROM batches $where");
    bindParams($stmt, $params);
    $stmt->execute();
    return (int)$stmt->fetchColumn();
}

function fetchLatestMeasurements(PDO $pdo, int $limit, int $offset, string $deviceId, string $range): array {
    [$where, $params] = buildWhereClause($deviceId, 'server_received_at', $range);

    $sql = "
        SELECT
            id,
            batch_id,
            device_id,
            seq,
            device_timestamp_utc,
            CASE
                WHEN time_valid = 1 AND device_timestamp_utc > 0 THEN FROM_UNIXTIME(device_timestamp_utc)
                ELSE NULL
            END AS device_time,
            server_received_at,
            FROM_UNIXTIME(server_received_at) AS server_time,
            CASE
                WHEN time_valid = 1 AND device_timestamp_utc > 0 THEN (server_received_at - device_timestamp_utc)
                ELSE NULL
            END AS delay_s,
            time_valid,
            uptime_s,
            temperature,
            humidity
        FROM measurements
        $where
        ORDER BY id DESC
        LIMIT :limit OFFSET :offset
    ";

    $stmt = $pdo->prepare($sql);
    bindParams($stmt, $params);
    $stmt->bindValue(':limit', $limit, PDO::PARAM_INT);
    $stmt->bindValue(':offset', $offset, PDO::PARAM_INT);
    $stmt->execute();

    return $stmt->fetchAll();
}

function countMeasurements(PDO $pdo, string $deviceId, string $range): int {
    [$where, $params] = buildWhereClause($deviceId, 'server_received_at', $range);
    $stmt = $pdo->prepare("SELECT COUNT(*) FROM measurements $where");
    bindParams($stmt, $params);
    $stmt->execute();
    return (int)$stmt->fetchColumn();
}

function fetchMeasurementsForCsv(PDO $pdo, string $deviceId, string $range): array {
    [$where, $params] = buildWhereClause($deviceId, 'server_received_at', $range);

    $sql = "
        SELECT
            id,
            batch_id,
            device_id,
            seq,
            device_timestamp_utc,
            CASE
                WHEN time_valid = 1 AND device_timestamp_utc > 0 THEN FROM_UNIXTIME(device_timestamp_utc)
                ELSE NULL
            END AS device_time,
            server_received_at,
            FROM_UNIXTIME(server_received_at) AS server_time,
            CASE
                WHEN time_valid = 1 AND device_timestamp_utc > 0 THEN (server_received_at - device_timestamp_utc)
                ELSE NULL
            END AS delay_s,
            time_valid,
            uptime_s,
            temperature,
            humidity
        FROM measurements
        $where
        ORDER BY id DESC
        LIMIT 50000
    ";

    $stmt = $pdo->prepare($sql);
    bindParams($stmt, $params);
    $stmt->execute();

    return $stmt->fetchAll();
}

function fetchTimeSeriesPoints(PDO $pdo, string $deviceId, string $range, string $resolution): array {
    if ($resolution === '10min') {
        $bucketEpochExpr = "server_received_at - (server_received_at % 600)";
    } else {
        $bucketEpochExpr = "server_received_at - (server_received_at % 3600)";
    }

    [$where, $params] = buildWhereClause($deviceId, 'server_received_at', $range);

    $sql = "
        SELECT
            {$bucketEpochExpr} AS bucket_epoch,
            FROM_UNIXTIME({$bucketEpochExpr}) AS bucket,
            COUNT(*) AS samples,
            AVG(temperature) AS avg_temperature,
            AVG(humidity) AS avg_humidity,
            AVG(CASE WHEN time_valid = 1 AND device_timestamp_utc > 0 THEN (server_received_at - device_timestamp_utc) END) AS avg_delay_s,
            SUM(CASE WHEN time_valid = 1 THEN 1 ELSE 0 END) AS valid_count,
            COUNT(*) AS total_count
        FROM measurements
        $where
        GROUP BY bucket_epoch
        ORDER BY bucket_epoch ASC
    ";

    $stmt = $pdo->prepare($sql);
    bindParams($stmt, $params);
    $stmt->execute();

    return $stmt->fetchAll();
}

function renderPagination(string $type, int $page, int $pageSize, int $totalItems, array $query): string {
    $totalPages = max(1, (int)ceil($totalItems / $pageSize));

    if ($totalPages <= 1) {
        return '';
    }

    $page = max(1, min($page, $totalPages));
    $html = '<div class="pagination">';

    if ($page > 1) {
        $prevQuery = $query;
        $prevQuery[$type . '_page'] = $page - 1;
        $html .= '<a class="page-link" href="?' . h(hq($prevQuery)) . '">← Föregående</a>';
    } else {
        $html .= '<span class="page-link disabled">← Föregående</span>';
    }

    $html .= '<span class="page-meta">Sida ' . h((string)$page) . ' av ' . h((string)$totalPages) . ' • ' . h((string)$totalItems) . ' rader</span>';

    if ($page < $totalPages) {
        $nextQuery = $query;
        $nextQuery[$type . '_page'] = $page + 1;
        $html .= '<a class="page-link" href="?' . h(hq($nextQuery)) . '">Nästa →</a>';
    } else {
        $html .= '<span class="page-link disabled">Nästa →</span>';
    }

    $html .= '</div>';
    return $html;
}

$selectedDevice = isset($_GET['device']) ? trim((string)$_GET['device']) : ($config['default_device'] ?? '');
$selectedSeries = selectedSeriesFromGet();
$selectedRange = selectedTimeRange();
$selectedResolution = selectedResolution();
$exportCsv = isset($_GET['export']) && $_GET['export'] === 'csv';

$batchPage = max(1, (int)($_GET['batch_page'] ?? 1));
$measurementPage = max(1, (int)($_GET['measurement_page'] ?? 1));
$batchPageSize = (int)($config['page_size_batches'] ?? 20);
$measurementPageSize = (int)($config['page_size_measurements'] ?? 100);

$errorMessage = '';
$devices = [];
$summary = [];
$batches = [];
$measurements = [];
$seriesPoints = [];
$batchTotal = 0;
$measurementTotal = 0;

try {
    $pdo = getPdo($config);
    $devices = fetchDevices($pdo);

    if ($selectedDevice !== '' && !in_array($selectedDevice, $devices, true)) {
        $selectedDevice = '';
    }

    if ($exportCsv) {
        $csvRows = fetchMeasurementsForCsv($pdo, $selectedDevice, $selectedRange);
        $filename = 'telemetry_' . $selectedRange . '_' . $selectedResolution . ($selectedDevice !== '' ? '_' . preg_replace('/[^a-zA-Z0-9_-]/', '_', $selectedDevice) : '') . '_' . date('Ymd_His') . '.csv';

        header('Content-Type: text/csv; charset=utf-8');
        header('Content-Disposition: attachment; filename="' . $filename . '"');

        $out = fopen('php://output', 'w');
        fputcsv($out, ['id', 'batch_id', 'device_id', 'seq', 'device_timestamp_utc', 'device_time', 'server_received_at', 'server_time', 'delay_s', 'time_valid', 'uptime_s', 'temperature', 'humidity']);

        foreach ($csvRows as $row) {
            fputcsv($out, [
                $row['id'],
                $row['batch_id'],
                $row['device_id'],
                $row['seq'],
                $row['device_timestamp_utc'],
                $row['device_time'],
                $row['server_received_at'],
                $row['server_time'],
                $row['delay_s'],
                $row['time_valid'],
                $row['uptime_s'],
                $row['temperature'],
                $row['humidity'],
            ]);
        }

        fclose($out);
        exit;
    }

    $summary = fetchSummary($pdo, $selectedDevice, $selectedRange);
    $batchTotal = countBatches($pdo, $selectedDevice, $selectedRange);
    $measurementTotal = countMeasurements($pdo, $selectedDevice, $selectedRange);

    $batchPage = min($batchPage, max(1, (int)ceil($batchTotal / $batchPageSize)));
    $measurementPage = min($measurementPage, max(1, (int)ceil($measurementTotal / $measurementPageSize)));

    $batches = fetchLatestBatches($pdo, $batchPageSize, ($batchPage - 1) * $batchPageSize, $selectedDevice, $selectedRange);
    $measurements = fetchLatestMeasurements($pdo, $measurementPageSize, ($measurementPage - 1) * $measurementPageSize, $selectedDevice, $selectedRange);
    $seriesPoints = fetchTimeSeriesPoints($pdo, $selectedDevice, $selectedRange, $selectedResolution);
} catch (Throwable $e) {
    $errorMessage = $e->getMessage();
}

$chartData = [
    'labels' => array_map(static fn($r) => (string)$r['bucket'], $seriesPoints),
    'temperature' => array_map(static fn($r) => $r['avg_temperature'] !== null ? (float)$r['avg_temperature'] : null, $seriesPoints),
    'humidity' => array_map(static fn($r) => $r['avg_humidity'] !== null ? (float)$r['avg_humidity'] : null, $seriesPoints),
    'delay' => array_map(static fn($r) => $r['avg_delay_s'] !== null ? (float)$r['avg_delay_s'] : null, $seriesPoints),
    'valid_ratio' => array_map(static fn($r) => ((int)$r['total_count'] > 0 ? ((float)$r['valid_count'] / (float)$r['total_count']) : 0.0), $seriesPoints),
    'visible' => $selectedSeries,
    'range' => $selectedRange,
    'resolution' => $selectedResolution,
];

$currentQuery = $_GET;
$csvUrl = '?' . hq(array_merge($currentQuery, ['export' => 'csv']));

$baseQuery = [
    'device' => $selectedDevice,
    'range' => $selectedRange,
    'res' => $selectedResolution,
    'series' => $selectedSeries,
    'batch_page' => $batchPage,
    'measurement_page' => $measurementPage,
];

$batchPagination = renderPagination('batch', $batchPage, $batchPageSize, $batchTotal, $baseQuery);
$measurementPagination = renderPagination('measurement', $measurementPage, $measurementPageSize, $measurementTotal, $baseQuery);

$rangeLabel = timeRangeLabel($selectedRange);
$resolutionLabel = resolutionLabel($selectedResolution);
?>
<!doctype html>
<html lang="sv">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Pico Telemetry Dashboard</title>
    <style>
        :root {
            --bg: #07101f;
            --bg-2: #0d1830;
            --panel: rgba(13, 24, 48, 0.92);
            --panel-2: rgba(20, 34, 67, 0.96);
            --text: #eef3ff;
            --muted: #a8b3d2;
            --line: rgba(255,255,255,0.08);
            --accent: #6ea8fe;
            --accent-2: #00d4ff;
            --temp: #e4572e;
            --hum: #1b9e77;
            --delay: #d99b00;
            --ok: #59d0a8;
            --warn: #ffca63;
            --bad: #ff7b7b;
            --shadow: 0 22px 60px rgba(0, 0, 0, 0.28);
            --radius: 22px;
        }

        * { box-sizing: border-box; }
        html, body { min-height: 100%; }

        body {
            margin: 0;
            font-family: Inter, system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif;
            color: var(--text);
            background:
                radial-gradient(circle at top left, rgba(110,168,254,0.16), transparent 26%),
                radial-gradient(circle at top right, rgba(0,212,255,0.12), transparent 22%),
                linear-gradient(180deg, var(--bg) 0%, var(--bg-2) 100%);
        }

        .wrap { width: min(1440px, calc(100% - 28px)); margin: 24px auto 40px; }

        .hero {
            display: grid;
            grid-template-columns: 1.4fr auto;
            gap: 18px;
            align-items: center;
            margin-bottom: 20px;
        }

        .hero-panel, .filter-panel, .card, .panel {
            background: var(--panel);
            border: 1px solid var(--line);
            box-shadow: var(--shadow);
            border-radius: var(--radius);
            backdrop-filter: blur(8px);
        }

        .hero-panel { padding: 22px 24px; }

        .eyebrow {
            display: inline-flex;
            gap: 8px;
            align-items: center;
            padding: 6px 12px;
            border-radius: 999px;
            background: rgba(255,255,255,0.05);
            border: 1px solid rgba(255,255,255,0.08);
            color: var(--muted);
            font-size: 12px;
            text-transform: uppercase;
            letter-spacing: 0.08em;
            margin-bottom: 12px;
        }

        h1 { margin: 0; font-size: clamp(28px, 3.2vw, 42px); line-height: 1.08; }
        .hero-sub { margin-top: 10px; color: var(--muted); font-size: 15px; max-width: 900px; }

        .filter-panel { padding: 16px; min-width: 420px; }
        .filter-panel form { display: grid; gap: 12px; }
        .filter-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }

        label { color: var(--muted); font-size: 13px; }

        select, button {
            width: 100%;
            border: 1px solid var(--line);
            border-radius: 14px;
            padding: 12px 14px;
            font-size: 14px;
        }

        select { background: var(--panel-2); color: var(--text); }

        button, .ghost-link {
            cursor: pointer;
            font-weight: 800;
            border: none;
            text-decoration: none;
            display: inline-flex;
            justify-content: center;
            align-items: center;
            background: linear-gradient(135deg, var(--accent), var(--accent-2));
            color: #04111f;
            box-shadow: 0 10px 30px rgba(110,168,254,0.25);
        }

        .ghost-link { padding: 12px 14px; border-radius: 14px; font-size: 14px; }
        .actions { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }

        .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 16px; margin-bottom: 18px; }

        .card { padding: 18px 20px; position: relative; overflow: hidden; }
        .card::after {
            content: '';
            position: absolute;
            inset: auto -20px -20px auto;
            width: 110px;
            height: 110px;
            background: radial-gradient(circle, rgba(255,255,255,0.08), transparent 68%);
            pointer-events: none;
        }

        .card .label { color: var(--muted); font-size: 13px; margin-bottom: 10px; }
        .card .value { font-size: clamp(26px, 2.4vw, 34px); font-weight: 800; }
        .card .small { margin-top: 8px; color: var(--muted); font-size: 13px; }

        .layout { display: grid; grid-template-columns: 1.3fr 0.9fr; gap: 18px; margin-bottom: 18px; }
        .panel { padding: 18px 20px; }
        .panel h2 { margin: 0 0 10px; font-size: 20px; }
        .panel-sub { color: var(--muted); font-size: 13px; margin-top: 0; margin-bottom: 14px; }

        .stats-list { display: grid; gap: 10px; }
        .stats-item { display: flex; justify-content: space-between; gap: 16px; padding: 10px 0; border-bottom: 1px dashed var(--line); }
        .stats-item:last-child { border-bottom: none; }

        .muted { color: var(--muted); }

        .status-chip {
            display: inline-flex;
            align-items: center;
            gap: 8px;
            padding: 7px 12px;
            border-radius: 999px;
            font-size: 12px;
            font-weight: 700;
            background: rgba(255,255,255,0.05);
            border: 1px solid rgba(255,255,255,0.08);
        }

        .status-chip.ok { color: var(--ok); }
        .status-chip.warn { color: var(--warn); }
        .status-chip.bad { color: var(--bad); }

        .executive-box {
            margin-top: 14px;
            padding: 14px 16px;
            border-radius: 18px;
            border: 1px solid var(--line);
            background: rgba(255,255,255,0.03);
        }

        .executive-box ul { margin: 8px 0 0 18px; padding: 0; }
        .executive-box li { margin-bottom: 6px; color: var(--muted); }

        .chart-toolbar {
            display: flex;
            flex-wrap: wrap;
            gap: 10px;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 8px;
        }

        .series-toggles, .zoom-controls {
            display: flex;
            flex-wrap: wrap;
            gap: 8px;
        }

        .series-toggle, .zoom-btn {
            width: auto;
            border-radius: 999px;
            border: 1px solid var(--line);
            background: rgba(255,255,255,0.04);
            color: var(--text);
            padding: 8px 12px;
            font-size: 13px;
            display: inline-flex;
            align-items: center;
            gap: 8px;
            cursor: pointer;
            box-shadow: none;
        }

        .series-toggle.active {
            box-shadow: inset 0 0 0 1px rgba(255,255,255,0.18);
            background: rgba(255,255,255,0.08);
        }

        .dot { width: 10px; height: 10px; border-radius: 999px; display: inline-block; }

        .chart-wrap {
            position: relative;
            width: 100%;
            min-height: 390px;
            height: 390px;
            background: #f4f6f8;
            border-radius: 18px;
            overflow: hidden;
            border: 1px solid rgba(255,255,255,0.16);
        }

        #telemetryChart { width: 100%; height: 100%; display: block; }

        .legend { display: flex; flex-wrap: wrap; gap: 14px; margin-top: 10px; color: var(--muted); font-size: 13px; }
        .legend-item { display: inline-flex; gap: 8px; align-items: center; }
        .legend-swatch { width: 12px; height: 12px; border-radius: 999px; }

        .table-panel { margin-bottom: 18px; }
        .table-scroll { overflow-x: auto; }

        table { width: 100%; border-collapse: collapse; font-size: 14px; }
        th, td { padding: 10px 8px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; white-space: nowrap; }
        th { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.06em; position: sticky; top: 0; background: rgba(13,24,48,0.98); }
        tr:hover td { background: rgba(255,255,255,0.03); }

        .status-ok { color: var(--ok); font-weight: 700; }
        .status-bad { color: var(--bad); font-weight: 700; }

        .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }

        .error {
            padding: 16px 18px;
            color: #ffd7d7;
            background: rgba(255, 123, 123, 0.12);
            border: 1px solid rgba(255, 123, 123, 0.32);
            border-radius: 18px;
            box-shadow: var(--shadow);
        }

        .pill {
            display: inline-flex;
            padding: 5px 10px;
            border-radius: 999px;
            background: rgba(255,255,255,0.06);
            border: 1px solid rgba(255,255,255,0.08);
            color: var(--muted);
            font-size: 12px;
        }

        .pagination {
            display: flex;
            justify-content: space-between;
            gap: 12px;
            align-items: center;
            margin-top: 14px;
            flex-wrap: wrap;
        }

        .page-link {
            padding: 10px 14px;
            border-radius: 12px;
            text-decoration: none;
            border: 1px solid var(--line);
            background: rgba(255,255,255,0.04);
            color: var(--text);
        }

        .page-link.disabled { opacity: 0.4; pointer-events: none; }
        .page-meta { color: var(--muted); font-size: 13px; }

        @media (max-width: 1180px) {
            .grid { grid-template-columns: repeat(2, 1fr); }
            .layout { grid-template-columns: 1fr; }
            .hero { grid-template-columns: 1fr; }
        }

        @media (max-width: 720px) {
            .wrap { width: min(100%, calc(100% - 18px)); }
            .grid { grid-template-columns: 1fr; }
            .chart-wrap { height: 320px; min-height: 320px; }
            .panel, .card, .hero-panel, .filter-panel { border-radius: 18px; }
            .chart-toolbar { align-items: flex-start; }
            .filter-panel { min-width: auto; }
            .filter-grid, .actions { grid-template-columns: 1fr; }
        }
    </style>
</head>

<body>
<div class="wrap">
    <div class="hero">
        <div class="hero-panel">
            <div class="eyebrow">Telemetry • Zoom • Tidsfilter • CSV export</div>
            <h1>Pico Telemetry Dashboard</h1>
            <div class="hero-sub">
                Dashboard för temperatur, luftfuktighet och delay. Visar <?= h($rangeLabel) ?> med <?= h($resolutionLabel) ?>.
            </div>
        </div>

        <div class="filter-panel">
            <form method="get" id="filterForm">
                <div class="filter-grid">
                    <div>
                        <label for="device">Enhet</label>
                        <select name="device" id="device">
                            <option value="">Alla enheter</option>
                            <?php foreach ($devices as $device): ?>
                                <option value="<?= h($device) ?>" <?= $device === $selectedDevice ? 'selected' : '' ?>>
                                    <?= h($device) ?>
                                </option>
                            <?php endforeach; ?>
                        </select>
                    </div>

                    <div>
                        <label for="range">Tidsfilter</label>
                        <select name="range" id="range">
                            <option value="24h" <?= $selectedRange === '24h' ? 'selected' : '' ?>>Senaste 24h</option>
                            <option value="7d" <?= $selectedRange === '7d' ? 'selected' : '' ?>>Senaste 7 dagar</option>
                            <option value="30d" <?= $selectedRange === '30d' ? 'selected' : '' ?>>Senaste 30 dagar</option>
                            <option value="all" <?= $selectedRange === 'all' ? 'selected' : '' ?>>All tillgänglig data</option>
                        </select>
                    </div>

                    <div>
                        <label for="res">Medelvärde</label>
                        <select name="res" id="res">
                            <option value="hour" <?= $selectedResolution === 'hour' ? 'selected' : '' ?>>Timvisa medel</option>
                            <option value="10min" <?= $selectedResolution === '10min' ? 'selected' : '' ?>>10-minuters medel</option>
                        </select>
                    </div>
                </div>

                <?php foreach (['temperature', 'humidity', 'delay'] as $seriesName): ?>
                    <input type="hidden" name="series[]" value="<?= h($seriesName) ?>" <?= in_array($seriesName, $selectedSeries, true) ? '' : 'disabled' ?> data-hidden-series="<?= h($seriesName) ?>">
                <?php endforeach; ?>

                <input type="hidden" name="batch_page" value="1">
                <input type="hidden" name="measurement_page" value="1">

                <div class="actions">
                    <button type="submit">Uppdatera dashboard</button>
                    <a class="ghost-link" href="<?= h($csvUrl) ?>">Exportera CSV</a>
                </div>
            </form>
        </div>
    </div>

    <?php if ($errorMessage !== ''): ?>
        <div class="error">
            <strong>Databasfel:</strong><br>
            <?= h($errorMessage) ?><br><br>
            Kontrollera <span class="mono">config.php</span>, tabellerna och kolumnnamnen.
        </div>
    <?php else: ?>

    <div class="grid">
        <div class="card">
            <div class="label">Status</div>
            <div class="value" style="font-size: 24px;">
                <span class="status-chip <?= h($summary['health_level'] ?? 'ok') ?>"><?= h($summary['health'] ?? 'Ok') ?></span>
            </div>
            <div class="small"><?= h($rangeLabel) ?> • <?= $selectedDevice !== '' ? h($selectedDevice) : 'Alla enheter' ?></div>
        </div>

        <div class="card">
            <div class="label">Mätningar i period</div>
            <div class="value"><?= h($summary['measurements_total'] ?? 0) ?></div>
            <div class="small">Visar sida <?= h($measurementPage) ?> i tabellen nedan</div>
        </div>

        <div class="card">
            <div class="label">Max queue i period</div>
            <div class="value"><?= h($summary['max_queue'] ?? 0) ?></div>
            <div class="small">Batchar i period: <?= h($summary['batch_count'] ?? 0) ?></div>
        </div>

        <div class="card">
            <div class="label">Dropped i period</div>
            <div class="value"><?= h($summary['dropped_delta'] ?? 0) ?></div>
            <div class="small">Aktuellt totalvärde: <?= h($summary['latest_dropped_total'] ?? 0) ?></div>
        </div>
    </div>

    <div class="layout">
        <div class="panel">
            <h2>Graf – <?= h($rangeLabel) ?></h2>
            <div class="panel-sub">Växla serier, zooma och panorera i grafen.</div>

            <div class="chart-toolbar">
                <div class="series-toggles" id="seriesToggles">
                    <button type="button" class="series-toggle <?= in_array('temperature', $selectedSeries, true) ? 'active' : '' ?>" data-series="temperature"><span class="dot" style="background: var(--temp)"></span>Temperatur</button>
                    <button type="button" class="series-toggle <?= in_array('humidity', $selectedSeries, true) ? 'active' : '' ?>" data-series="humidity"><span class="dot" style="background: var(--hum)"></span>Luftfuktighet</button>
                    <button type="button" class="series-toggle <?= in_array('delay', $selectedSeries, true) ? 'active' : '' ?>" data-series="delay"><span class="dot" style="background: var(--delay)"></span>Delay</button>
                </div>

                <div class="zoom-controls">
                    <button type="button" class="zoom-btn" data-zoom="in">Zooma in</button>
                    <button type="button" class="zoom-btn" data-zoom="out">Zooma ut</button>
                    <button type="button" class="zoom-btn" data-pan="left">←</button>
                    <button type="button" class="zoom-btn" data-pan="right">→</button>
                    <button type="button" class="zoom-btn" data-zoom="reset">Reset</button>
                </div>

                <span class="pill"><?= h($resolutionLabel) ?></span>
            </div>

            <div class="chart-wrap"><canvas id="telemetryChart"></canvas></div>

            <div class="legend">
                <div class="legend-item"><span class="legend-swatch" style="background: var(--temp)"></span> Temperatur °C</div>
                <div class="legend-item"><span class="legend-swatch" style="background: var(--hum)"></span> Luftfuktighet %RH</div>
                <div class="legend-item"><span class="legend-swatch" style="background: var(--delay)"></span> Delay s</div>
                <div class="legend-item">Svagare punkt/linje = lägre andel time_valid i bucket</div>
            </div>
        </div>

        <div class="panel">
            <h2>Executive summary</h2>
            <div class="panel-sub">Sammanfattning för <?= h($rangeLabel) ?>.</div>

            <div class="stats-list">
                <div class="stats-item"><span class="muted">Medeltemperatur</span><span><?= isset($summary['avg_temperature']) && $summary['avg_temperature'] !== null ? number_format((float)$summary['avg_temperature'], 2) . ' °C' : '–' ?></span></div>
                <div class="stats-item"><span class="muted">Medelfukt</span><span><?= isset($summary['avg_humidity']) && $summary['avg_humidity'] !== null ? number_format((float)$summary['avg_humidity'], 2) . ' %RH' : '–' ?></span></div>
                <div class="stats-item"><span class="muted">Medeldelay</span><span><?= isset($summary['avg_delay_s']) && $summary['avg_delay_s'] !== null ? number_format((float)$summary['avg_delay_s'], 1) . ' s' : '–' ?></span></div>
                <div class="stats-item"><span class="muted">Max delay</span><span><?= isset($summary['max_delay_s']) && $summary['max_delay_s'] !== null ? number_format((float)$summary['max_delay_s'], 1) . ' s' : '–' ?></span></div>
                <div class="stats-item"><span class="muted">Tid giltig</span><span><?= number_format((float)($summary['time_valid_ratio'] ?? 0), 1) ?> %</span></div>
                <div class="stats-item"><span class="muted">Första device-tid</span><span><?= !empty($summary['min_ts']) ? h(date('Y-m-d H:i:s', (int)$summary['min_ts'])) : '–' ?></span></div>
                <div class="stats-item"><span class="muted">Senaste device-tid</span><span><?= !empty($summary['max_ts']) ? h(date('Y-m-d H:i:s', (int)$summary['max_ts'])) : '–' ?></span></div>
            </div>

            <div class="executive-box">
                <strong>Kommentar</strong>
                <ul>
                    <li>Status bedöms som <strong><?= h($summary['health'] ?? 'Ok') ?></strong>.</li>
                    <li>Senaste batch time_valid: <strong><?= ((int)($summary['latest_batch']['time_valid'] ?? 0) === 1) ? 'Ja' : 'Nej' ?></strong>.</li>
                    <li>Högsta seq i period: <strong><?= h($summary['max_seq'] ?? '–') ?></strong>.</li>
                    <li>Lägsta seq i period: <strong><?= h($summary['min_seq'] ?? '–') ?></strong>.</li>
                </ul>
            </div>
        </div>
    </div>

    <div class="panel table-panel">
        <h2>Batchar – <?= h($rangeLabel) ?></h2>
        <div class="panel-sub">Pagination gör att sidan inte behöver rendera alla batchar samtidigt.</div>

        <div class="table-scroll">
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
                            <td class="<?= (int)$row['time_valid'] === 1 ? 'status-ok' : 'status-bad' ?>"><?= (int)$row['time_valid'] === 1 ? 'Ja' : 'Nej' ?></td>
                            <td><?= h($row['remote_ip']) ?></td>
                            <td><?= h($row['raw_json_length']) ?></td>
                        </tr>
                    <?php endforeach; ?>
                <?php endif; ?>
                </tbody>
            </table>
        </div>

        <?= $batchPagination ?>
    </div>

    <div class="panel table-panel">
        <h2>Mätningar – <?= h($rangeLabel) ?></h2>
        <div class="panel-sub">CSV-exporten använder samma filter som dashboarden. Tabellen är paginerad för bättre prestanda.</div>

        <div class="table-scroll">
            <table>
                <thead>
                    <tr>
                        <th>ID</th>
                        <th>Enhet</th>
                        <th>Seq</th>
                        <th>Device-tid</th>
                        <th>Server-tid</th>
                        <th>Delay (s)</th>
                        <th>Temp °C</th>
                        <th>Hum %RH</th>
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
                            <td><?= h($row['device_time'] ?? '–') ?></td>
                            <td><?= h($row['server_time']) ?></td>
                            <td><?= $row['delay_s'] !== null ? h(number_format((float)$row['delay_s'], 1)) : '–' ?></td>
                            <td><?= $row['temperature'] !== null ? h(number_format((float)$row['temperature'], 2)) : '–' ?></td>
                            <td><?= $row['humidity'] !== null ? h(number_format((float)$row['humidity'], 2)) : '–' ?></td>
                            <td><?= h($row['uptime_s']) ?></td>
                            <td class="<?= (int)$row['time_valid'] === 1 ? 'status-ok' : 'status-bad' ?>"><?= (int)$row['time_valid'] === 1 ? 'Ja' : 'Nej' ?></td>
                        </tr>
                    <?php endforeach; ?>
                <?php endif; ?>
                </tbody>
            </table>
        </div>

        <?= $measurementPagination ?>
    </div>

    <?php endif; ?>
</div>

<?php if ($errorMessage === ''): ?>
<script>
(() => {
    const chartData = <?= json_encode($chartData, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_HEX_TAG | JSON_HEX_APOS | JSON_HEX_QUOT | JSON_HEX_AMP) ?>;

    const canvas = document.getElementById('telemetryChart');
    const toggleContainer = document.getElementById('seriesToggles');
    const hiddenInputs = Array.from(document.querySelectorAll('[data-hidden-series]'));
    const visible = new Set(chartData.visible || ['temperature', 'humidity', 'delay']);

    let zoomStart = 0;
    let zoomEnd = Math.max(0, (chartData.labels || []).length - 1);

    function syncHiddenInputs() {
        hiddenInputs.forEach(input => {
            input.disabled = !visible.has(input.getAttribute('data-hidden-series'));
        });
    }

    syncHiddenInputs();

    if (toggleContainer) {
        toggleContainer.addEventListener('click', (event) => {
            const btn = event.target.closest('.series-toggle');
            if (!btn) return;

            const series = btn.getAttribute('data-series');
            if (!series) return;

            if (visible.has(series) && visible.size > 1) {
                visible.delete(series);
                btn.classList.remove('active');
            } else if (!visible.has(series)) {
                visible.add(series);
                btn.classList.add('active');
            }

            syncHiddenInputs();
            resizeAndDraw();
        });
    }

    document.querySelectorAll('.zoom-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            const labels = chartData.labels || [];
            const n = labels.length;
            if (n < 2) return;

            const mode = btn.getAttribute('data-zoom');
            const pan = btn.getAttribute('data-pan');
            const span = zoomEnd - zoomStart + 1;
            const center = Math.floor((zoomStart + zoomEnd) / 2);

            if (mode === 'reset') {
                zoomStart = 0;
                zoomEnd = n - 1;
            } else if (mode === 'in') {
                const newSpan = Math.max(4, Math.floor(span * 0.6));
                zoomStart = Math.max(0, center - Math.floor(newSpan / 2));
                zoomEnd = Math.min(n - 1, zoomStart + newSpan - 1);
                zoomStart = Math.max(0, zoomEnd - newSpan + 1);
            } else if (mode === 'out') {
                const newSpan = Math.min(n, Math.ceil(span * 1.6));
                zoomStart = Math.max(0, center - Math.floor(newSpan / 2));
                zoomEnd = Math.min(n - 1, zoomStart + newSpan - 1);
                zoomStart = Math.max(0, zoomEnd - newSpan + 1);
            } else if (pan === 'left') {
                const shift = Math.max(1, Math.floor(span * 0.25));
                zoomStart = Math.max(0, zoomStart - shift);
                zoomEnd = Math.min(n - 1, zoomStart + span - 1);
            } else if (pan === 'right') {
                const shift = Math.max(1, Math.floor(span * 0.25));
                zoomEnd = Math.min(n - 1, zoomEnd + shift);
                zoomStart = Math.max(0, zoomEnd - span + 1);
            }

            resizeAndDraw();
        });
    });

    if (!canvas) return;

    const css = getComputedStyle(document.documentElement);

    const COLORS = {
        temperature: css.getPropertyValue('--temp').trim() || '#e4572e',
        humidity: css.getPropertyValue('--hum').trim() || '#1b9e77',
        delay: css.getPropertyValue('--delay').trim() || '#d99b00',
        text: '#1f2933',
        muted: '#5f6b7a',
        line: '#d4d9e2',
        axis: '#2f3945',
        plotBg: '#ffffff',
        canvasBg: '#f4f6f8'
    };

    function sanitizeSeries(values) {
        return values.map(v => (v === null || Number.isNaN(Number(v))) ? null : Number(v));
    }

    function sliceArray(arr) {
        return arr.slice(zoomStart, zoomEnd + 1);
    }

    function activeSeries() {
        return [
            { key: 'temperature', name: 'Temperatur °C', color: COLORS.temperature, unit: '°C', values: sanitizeSeries(chartData.temperature || []) },
            { key: 'humidity', name: 'Luftfuktighet %RH', color: COLORS.humidity, unit: '%RH', values: sanitizeSeries(chartData.humidity || []) },
            { key: 'delay', name: 'Delay s', color: COLORS.delay, unit: 's', values: sanitizeSeries(chartData.delay || []) }
        ].filter(s => visible.has(s.key));
    }

    function alphaFromValidRatio(index) {
        const ratio = (chartData.valid_ratio || [])[index] ?? 1;
        return 0.35 + ratio * 0.65;
    }

    function resizeAndDraw() {
        const ctx = canvas.getContext('2d');
        const ratio = window.devicePixelRatio || 1;
        const width = canvas.clientWidth || 800;
        const height = canvas.clientHeight || 390;

        canvas.width = Math.floor(width * ratio);
        canvas.height = Math.floor(height * ratio);
        ctx.setTransform(ratio, 0, 0, ratio, 0, 0);

        drawChart(ctx, width, height);
    }

    function drawChart(ctx, width, height) {
        const labelsFull = chartData.labels || [];
        const labels = sliceArray(labelsFull);
        const series = activeSeries().map(s => ({ ...s, values: sliceArray(s.values) }));

        const allValues = series.flatMap(s => s.values).filter(v => v !== null);

        ctx.clearRect(0, 0, width, height);

        ctx.fillStyle = COLORS.canvasBg;
        ctx.fillRect(0, 0, width, height);

        if (labels.length === 0 || allValues.length === 0) {
            ctx.fillStyle = COLORS.muted;
            ctx.font = '14px system-ui';
            ctx.fillText('Ingen grafdata för vald period.', 20, 30);
            return;
        }

        const padding = { top: 42, right: 24, bottom: 72, left: 74 };
        const plotX = padding.left;
        const plotY = padding.top;
        const plotW = Math.max(10, width - padding.left - padding.right);
        const plotH = Math.max(10, height - padding.top - padding.bottom);

        const rawMin = Math.min(...allValues);
        const rawMax = Math.max(...allValues);
        const range = rawMax - rawMin || 1;
        const minVal = rawMin - range * 0.12;
        const maxVal = rawMax + range * 0.18;

        ctx.fillStyle = COLORS.plotBg;
        ctx.fillRect(plotX, plotY, plotW, plotH);

        ctx.strokeStyle = COLORS.line;
        ctx.lineWidth = 1;

        ctx.font = '12px system-ui';
        ctx.fillStyle = COLORS.axis;

        for (let i = 0; i <= 5; i++) {
            const y = plotY + (plotH / 5) * i;
            const value = maxVal - ((maxVal - minVal) / 5) * i;

            ctx.beginPath();
            ctx.moveTo(plotX, y);
            ctx.lineTo(plotX + plotW, y);
            ctx.stroke();

            ctx.fillStyle = COLORS.axis;
            ctx.fillText(value.toFixed(1), 12, y + 4);
        }

        ctx.strokeStyle = COLORS.axis;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.moveTo(plotX, plotY);
        ctx.lineTo(plotX, plotY + plotH);
        ctx.lineTo(plotX + plotW, plotY + plotH);
        ctx.stroke();

        const step = labels.length > 1 ? plotW / (labels.length - 1) : plotW;
        const visibleLabelStep = Math.max(1, Math.ceil(labels.length / 8));

        for (let i = 0; i < labels.length; i++) {
            const x = plotX + step * i;

            if (i % visibleLabelStep === 0 || i === labels.length - 1) {
                const label = String(labels[i]).slice(5, chartData.resolution === '10min' ? 16 : 16);

                ctx.save();
                ctx.translate(x, plotY + plotH + 24);
                ctx.rotate(-Math.PI / 6);
                ctx.fillStyle = COLORS.axis;
                ctx.font = '12px system-ui';
                ctx.fillText(label, 0, 0);
                ctx.restore();
            }
        }

        const toY = (v) => plotY + plotH - ((v - minVal) / (maxVal - minVal)) * plotH;

        series.forEach((s) => {
            ctx.lineWidth = 2.8;
            ctx.beginPath();

            let started = false;

            s.values.forEach((v, i) => {
                if (v === null) return;

                const originalIndex = zoomStart + i;
                const x = plotX + step * i;
                const y = toY(v);
                const a = alphaFromValidRatio(originalIndex);

                ctx.strokeStyle = hexToRgba(s.color, a);

                if (!started) {
                    ctx.moveTo(x, y);
                    started = true;
                } else {
                    ctx.lineTo(x, y);
                }
            });

            ctx.stroke();

            s.values.forEach((v, i) => {
                if (v === null) return;

                const originalIndex = zoomStart + i;
                const x = plotX + step * i;
                const y = toY(v);
                const a = alphaFromValidRatio(originalIndex);

                ctx.fillStyle = hexToRgba(s.color, a);
                ctx.beginPath();
                ctx.arc(x, y, 3.6, 0, Math.PI * 2);
                ctx.fill();
            });
        });

        ctx.fillStyle = COLORS.axis;
        ctx.font = '700 13px system-ui';
        ctx.fillText('Temp °C / RH % / Delay s', 12, 20);
        ctx.fillText('Tid', plotX + plotW - 24, height - 10);

        ctx.fillStyle = COLORS.muted;
        ctx.font = '12px system-ui';
        ctx.fillText(`Zoom: ${zoomStart + 1}–${zoomEnd + 1} av ${labelsFull.length}`, plotX, height - 10);
    }

    function hexToRgba(hex, alpha) {
        if (!hex.startsWith('#')) return hex;

        const clean = hex.replace('#', '');
        const bigint = parseInt(clean.length === 3 ? clean.split('').map(c => c + c).join('') : clean, 16);

        const r = (bigint >> 16) & 255;
        const g = (bigint >> 8) & 255;
        const b = bigint & 255;

        return `rgba(${r},${g},${b},${alpha})`;
    }

    window.addEventListener('resize', resizeAndDraw);
    resizeAndDraw();
})();
</script>
<?php endif; ?>

</body>
</html>
