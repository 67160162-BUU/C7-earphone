<?php
/**
 * ============================================================================
 * API Endpoint: /C7/api/save_angle.php
 * Purpose:
 *   Receives posture angle data from ESP32 (via HTTP POST JSON or GET/POST params)
 *   and saves to DUAL DATABASE STORAGE simultaneously:
 *     1. XAMPP MySQL Database: `c7_posture_db` -> table `posture_logs`
 *        (Auto-creates database & table if not yet created!)
 *     2. Local Project CSV & JSON Files:
 *        - `/Applications/XAMPP/xamppfiles/htdocs/C7/api/posture_logs.csv`
 *        - `/Applications/XAMPP/xamppfiles/htdocs/C7/api/latest_angle.json`
 * ============================================================================
 */

header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: POST, GET, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

date_default_timezone_set('Asia/Bangkok');

// 1. Read input from JSON body (sent by ESP32) or $_POST / $_GET parameters
$rawBody = file_get_contents('php://input');
$jsonData = json_decode($rawBody, true);
$input = is_array($jsonData) ? $jsonData : array_merge($_GET, $_POST);

if (!isset($input['c7_angle']) && !isset($input['head_pitch']) && !isset($input['cva_angle'])) {
    http_response_code(400);
    echo json_encode([
        'status' => 'error',
        'message' => 'Missing angle parameters. Send JSON: {"c7_angle":-9.0,"head_pitch":0.0,"head_roll":0.0,"head_yaw":0.0,"cva_angle":52.05}'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

$c7_angle   = isset($input['c7_angle'])   ? round((float)$input['c7_angle'], 2)   : -9.00;
$head_pitch = isset($input['head_pitch']) ? round((float)$input['head_pitch'], 2) : 0.00;
$head_roll  = isset($input['head_roll'])  ? round((float)$input['head_roll'], 2)  : 0.00;
$head_yaw   = isset($input['head_yaw'])   ? round((float)$input['head_yaw'], 2)   : 0.00;

// Calculate CVA if not explicitly provided
if (isset($input['cva_angle']) && is_numeric($input['cva_angle'])) {
    $cva_angle = round((float)$input['cva_angle'], 2);
} else {
    $neckTiltDeg = 4.2 + ($c7_angle * 1.02) + max(0.0, $head_pitch * 0.16);
    $neckTiltRad = $neckTiltDeg * (M_PI / 180.0);
    $earY = 0.04 + 0.985 * cos($neckTiltRad) + 0.12;
    $earZ = 0.725 + 0.985 * sin($neckTiltRad) + 0.2505;
    $cva_angle = round(atan2($earY, max(0.15, $earZ)) * (180.0 / M_PI), 2);
}

$timestamp = date('Y-m-d H:i:s');
$mysqlSaved = false;
$mysqlId = null;
$mysqlError = null;

// ============================================================================
// STORAGE #1: Save to XAMPP MySQL (`c7_posture_db`.`posture_logs`)
// ============================================================================
try {
    $xamppSock = '/Applications/XAMPP/xamppfiles/var/mysql/mysql.sock';
    $dsnServer = file_exists($xamppSock)
        ? "mysql:unix_socket={$xamppSock};charset=utf8mb4"
        : 'mysql:host=127.0.0.1;port=3306;charset=utf8mb4';
    $pdo = new PDO($dsnServer, 'root', '', [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_TIMEOUT => 2
    ]);

    // Auto-create database & table if they do not exist yet
    $pdo->exec("CREATE DATABASE IF NOT EXISTS c7_posture_db CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
    $pdo->exec("USE c7_posture_db");
    $pdo->exec("
        CREATE TABLE IF NOT EXISTS posture_logs (
            id INT AUTO_INCREMENT PRIMARY KEY,
            c7_angle FLOAT NOT NULL,
            head_pitch FLOAT NOT NULL,
            head_roll FLOAT NOT NULL,
            head_yaw FLOAT NOT NULL,
            cva_angle FLOAT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    ");

    $stmt = $pdo->prepare("
        INSERT INTO posture_logs (c7_angle, head_pitch, head_roll, head_yaw, cva_angle, created_at)
        VALUES (:c7, :pitch, :roll, :yaw, :cva, :ts)
    ");
    $stmt->execute([
        ':c7'    => $c7_angle,
        ':pitch' => $head_pitch,
        ':roll'  => $head_roll,
        ':yaw'   => $head_yaw,
        ':cva'   => $cva_angle,
        ':ts'    => $timestamp
    ]);

    $mysqlId = (int)$pdo->lastInsertId();
    $mysqlSaved = true;
} catch (Throwable $e) {
    $mysqlError = $e->getMessage();
}

// ============================================================================
// STORAGE #2: Save to Local CSV (`posture_logs.csv`) & `latest_angle.json`
// ============================================================================
$csvFile    = __DIR__ . '/posture_logs.csv';
$latestFile = __DIR__ . '/latest_angle.json';

// Determine sequential ID if MySQL is offline
$prevLatest = @json_decode(@file_get_contents($latestFile), true);
$recordId   = $mysqlId !== null ? $mysqlId : (isset($prevLatest['id']) ? ((int)$prevLatest['id'] + 1) : 1);

$isNewCsv = !file_exists($csvFile) || filesize($csvFile) === 0;
$csvSaved = false;

if ($fp = @fopen($csvFile, 'a')) {
    if (flock($fp, LOCK_EX)) {
        if ($isNewCsv) {
            fputcsv($fp, ['id', 'c7_angle', 'head_pitch', 'head_roll', 'head_yaw', 'cva_angle', 'created_at']);
        }
        fputcsv($fp, [$recordId, $c7_angle, $head_pitch, $head_roll, $head_yaw, $cva_angle, $timestamp]);
        fflush($fp);
        flock($fp, LOCK_UN);
        $csvSaved = true;
    }
    fclose($fp);
}

$latestPayload = [
    'status'          => 'ok',
    'id'              => $recordId,
    'c7_angle'        => $c7_angle,
    'head_pitch'      => $head_pitch,
    'head_roll'       => $head_roll,
    'head_yaw'        => $head_yaw,
    'cva_angle'       => $cva_angle,
    'created_at'      => $timestamp,
    'mysql_saved'     => $mysqlSaved,
    'csv_saved'       => $csvSaved,
    'mysql_error'     => $mysqlError
];

@file_put_contents($latestFile, json_encode($latestPayload, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE));

echo json_encode($latestPayload, JSON_UNESCAPED_UNICODE);
