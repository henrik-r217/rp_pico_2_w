<?php
declare(strict_types=1);

/*
 * Database configuration for Pico Telemetry Dashboard.
 *
 * Upload this file next to index.php on your web hotel.
 * Then update index.php to include it with:
 *
 *     require __DIR__ . '/config.php';
 *
 * Recommended:
 * - Keep this file outside public version control.
 * - Set real database credentials before production use.
 */

return [
    'host' => 'mysql57.unoeuro.com',
    'port' => 3306,
    'dbname' => 'lindenet_se_db_pico_telemetry',
    'user' => 'lindenet_se',
    'pass' => '34d1hd6g67',
    'charset' => 'utf8mb4',

    // UI settings
    'page_size_batches' => 20,
    'page_size_measurements' => 100,
    'default_device' => '',
    'timezone' => 'UTC',
];

