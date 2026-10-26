-- ============================================================================
-- C7 Office Syndrome / Forward Head Posture Database Schema
-- Database: c7_posture_db
-- Table   : posture_logs (Stores only angle degrees and timestamp)
-- ============================================================================

CREATE DATABASE IF NOT EXISTS c7_posture_db
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE c7_posture_db;

CREATE TABLE IF NOT EXISTS posture_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    c7_angle FLOAT NOT NULL COMMENT 'มุมเอียงฐานคอ C7 (องศา) -> simState.targetC7',
    head_pitch FLOAT NOT NULL COMMENT 'มุมก้ม/เงยศีรษะเฉลี่ยซ้าย-ขวา (องศา) -> simState.targetPitch',
    head_roll FLOAT NOT NULL COMMENT 'มุมเอียงคอซ้าย/ขวา (องศา) -> simState.targetRoll',
    head_yaw FLOAT NOT NULL COMMENT 'มุมหันหน้าซ้าย/ขวา (องศา) -> simState.targetYaw',
    cva_angle FLOAT NOT NULL COMMENT 'มุม CVA ที่คำนวณได้ (องศา)',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT 'เวลาที่บันทึก (Timestamp)'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ข้อมูลเริ่มต้น (Baseline Origin ของกลุ่มคนปกติ 52.05°)
INSERT INTO posture_logs (c7_angle, head_pitch, head_roll, head_yaw, cva_angle)
VALUES (-9.00, 0.00, 0.00, 0.00, 52.05);
