#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_BNO08x.h>

// ============================================================================
// C7-BIOSYNC // USB SERIAL DIRECT MODE (AUTO-ZERO & BREADBOARD READY)
// ตั้งศูนย์อัตโนมัติ (Auto-Zero Tare) ทันทีที่เปิดเครื่อง ไม่ว่าจะวางบอร์ดนอนราบหรือตั้งฉาก
// ============================================================================

#define I2C_SDA_PIN       21
#define I2C_SCL_PIN       22
#define HAPTIC_MOTOR_PIN  25
#define BOOT_BUTTON_PIN   0     // ปุ่ม BOOT บนบอร์ด ESP32 (กดเพื่อตั้งศูนย์ 0° ท่านั่งตรง)

#define BNO_LEFT_ADDR     0x4A  // BNO085 ตัวที่ 1 (ขา DI/ADDR ปล่อยว่าง หรือต่อ GND)
#define BNO_RIGHT_ADDR    0x4B  // BNO085 ตัวที่ 2 (ขา DI/ADDR ต่อเข้า 3.3V)
#define MPU6050_C7_ADDR   0x68  // เซ็นเซอร์เสริมที่ C7 (ถ้าเสียบเพิ่ม)

Adafruit_BNO08x *bnoLeft  = nullptr;
Adafruit_BNO08x *bnoRight = nullptr;
sh2_SensorValue_t sensorValueL;
sh2_SensorValue_t sensorValueR;

bool hasLeftEar  = false;
bool hasRightEar = false;
bool hasC7Mpu    = false;
bool autoZeroDone = false;

// ค่ามุมดิบจากเซ็นเซอร์
float pitchL = 0.0f, rollL = 0.0f, yawL = 0.0f;
float pitchR = 0.0f, rollR = 0.0f, yawR = 0.0f;
float rawHeadPitch = 0.0f, rawHeadRoll = 0.0f, rawHeadYaw = 0.0f;
float rawC7Angle   = 0.0f;

// ค่า Offset สำหรับตั้งศูนย์ (Zero Calibration)
float offsetPitch = 0.0f, offsetRoll = 0.0f, offsetYaw = 0.0f, offsetC7 = 0.0f;

// ค่ามุมสุทธิที่ส่งออกทางสาย USB
float headPitch = 0.0f, headRoll = 0.0f, headYaw = 0.0f;
float c7Angle   = -9.0f;
float cvaAngle  = 52.05f;

unsigned long lastSendMs = 0;
const unsigned long SEND_INTERVAL_MS = 200; // ส่งข้อมูลผ่านสาย USB ทุก 200 ms (5 Hz)

float wrapDeg(float d) {
  while (d > 180.0f) d -= 360.0f;
  while (d < -180.0f) d += 360.0f;
  return d;
}

float clampDeg(float v, float minV, float maxV) {
  if (v < minV) return minV;
  if (v > maxV) return maxV;
  return v;
}

bool isI2CDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

void enableBnoReports(Adafruit_BNO08x *bno) {
  if (!bno) return;
  bno->enableReport(SH2_ARVR_STABILIZED_RV, 50000); // 20 Hz (50ms) ไม่ให้กวนบัส I2C บน Breadboard
}

// แปลง Quaternion เป็นมุม Pitch (ก้ม/เงย), Roll (เอียงซ้าย/ขวา), Yaw (หันซ้าย/ขวา) ที่ถูกต้อง
void quaternionToEulerDeg(float qw, float qx, float qy, float qz, float &pitch, float &roll, float &yaw) {
  // Pitch (แกนก้ม/เงย)
  float sinp = 2.0f * (qw * qx + qy * qz);
  float cosp = 1.0f - 2.0f * (qx * qx + qy * qy);
  pitch = atan2f(sinp, cosp) * (180.0f / PI);

  // Roll (แกนเอียงคอซ้าย/ขวา)
  float sinr = 2.0f * (qw * qy - qz * qx);
  roll = (fabs(sinr) >= 1.0f) ? copysignf(90.0f, sinr) : asinf(sinr) * (180.0f / PI);

  // Yaw (แกนหันหน้าซ้าย/ขวา)
  float siny = 2.0f * (qw * qz + qx * qy);
  float cosy = 1.0f - 2.0f * (qy * qy + qz * qz);
  yaw = atan2f(siny, cosy) * (180.0f / PI);
}

bool initMpu6050AtC7() {
  if (!isI2CDevicePresent(MPU6050_C7_ADDR)) return false;
  Wire.beginTransmission(MPU6050_C7_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  return (Wire.endTransmission() == 0);
}

// อ่านมุมก้มเงยจาก MPU6050 โดยรองรับการวางนอนราบบน Breadboard (เทียบแกน X กับ Z)
float readC7PitchFromMpu6050() {
  Wire.beginTransmission(MPU6050_C7_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return rawC7Angle;

  Wire.requestFrom((uint8_t)MPU6050_C7_ADDR, (uint8_t)6, (uint8_t)true);
  if (Wire.available() < 6) return rawC7Angle;

  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();

  // ใช้ atan2(ax, az) เพื่อให้ตอนวางบอร์ดนอนราบบนโต๊ะมีค่ามุมใกล้ 0° (ไม่พุ่งไป +81°)
  return atan2f((float)ax, (float)az) * (180.0f / PI);
}

float calculateCVA(float c7Deg, float headPitchDeg) {
  float neckTiltDeg = 4.2f + c7Deg * 1.02f + max(0.0f, headPitchDeg * 0.16f);
  float neckTiltRad = neckTiltDeg * (PI / 180.0f);

  float earY = 0.04f + 0.985f * cosf(neckTiltRad) + 0.12f;
  float earZ = 0.725f + 0.985f * sinf(neckTiltRad) + 0.2505f;

  return atan2f(earY, max(0.15f, earZ)) * (180.0f / PI);
}

void calibrateNeutralPosture() {
  offsetPitch = rawHeadPitch;
  offsetRoll  = rawHeadRoll;
  offsetYaw   = rawHeadYaw;
  offsetC7    = rawC7Angle;
  autoZeroDone = true;
}

void setup() {
  WiFi.mode(WIFI_OFF);
  btStop();

  Serial.begin(115200);
  delay(800);

  Serial.println("\n============================================");
  Serial.println("🚀 C7-BIOSYNC (AUTO-ZERO CALIBRATED MODE)");
  Serial.println("============================================");

  pinMode(HAPTIC_MOTOR_PIN, OUTPUT);
  digitalWrite(HAPTIC_MOTOR_PIN, LOW);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(25); // ป้องกันบอร์ดค้างกรณีสายจัมเปอร์บน Breadboard ขยับหลวม
  delay(150);

  if (isI2CDevicePresent(BNO_LEFT_ADDR)) {
    bnoLeft = new Adafruit_BNO08x(-1);
    if (bnoLeft->begin_I2C(BNO_LEFT_ADDR, &Wire)) {
      hasLeftEar = true;
      enableBnoReports(bnoLeft);
      Serial.println("✅ [OK] BNO085 Left Ear (0x4A) Connected!");
    }
  }

  if (isI2CDevicePresent(BNO_RIGHT_ADDR)) {
    bnoRight = new Adafruit_BNO08x(-1);
    if (bnoRight->begin_I2C(BNO_RIGHT_ADDR, &Wire)) {
      hasRightEar = true;
      enableBnoReports(bnoRight);
      Serial.println("✅ [OK] BNO085 Right Ear (0x4B) Connected!");
    }
  }

  hasC7Mpu = initMpu6050AtC7();
  if (hasC7Mpu) {
    Serial.println("✅ [OK] MPU6050 at C7 (0x68) Connected!");
  }
}

void loop() {
  bool gotNewSample = false;

  if (hasLeftEar && bnoLeft) {
    if (bnoLeft->wasReset()) enableBnoReports(bnoLeft);
    if (bnoLeft->getSensorEvent(&sensorValueL)) {
      if (sensorValueL.sensorId == SH2_ARVR_STABILIZED_RV) {
        quaternionToEulerDeg(
          sensorValueL.un.arvrStabilizedRV.real,
          sensorValueL.un.arvrStabilizedRV.i,
          sensorValueL.un.arvrStabilizedRV.j,
          sensorValueL.un.arvrStabilizedRV.k,
          pitchL, rollL, yawL
        );
        gotNewSample = true;
      }
    }
  }

  if (hasRightEar && bnoRight) {
    if (bnoRight->wasReset()) enableBnoReports(bnoRight);
    if (bnoRight->getSensorEvent(&sensorValueR)) {
      if (sensorValueR.sensorId == SH2_ARVR_STABILIZED_RV) {
        quaternionToEulerDeg(
          sensorValueR.un.arvrStabilizedRV.real,
          sensorValueR.un.arvrStabilizedRV.i,
          sensorValueR.un.arvrStabilizedRV.j,
          sensorValueR.un.arvrStabilizedRV.k,
          pitchR, rollR, yawR
        );
        gotNewSample = true;
      }
    }
  }

  if (hasLeftEar && hasRightEar) {
    rawHeadPitch = (pitchL + pitchR) * 0.5f;
    rawHeadRoll  = (rollL  + rollR)  * 0.5f;
    rawHeadYaw   = (yawL   + yawR)   * 0.5f;
  } else if (hasLeftEar) {
    rawHeadPitch = pitchL; rawHeadRoll = rollL; rawHeadYaw = yawL;
  } else if (hasRightEar) {
    rawHeadPitch = pitchR; rawHeadRoll = rollR; rawHeadYaw = yawR;
  }

  if (hasC7Mpu) {
    rawC7Angle = readC7PitchFromMpu6050();
  }

  // ตั้งศูนย์อัตโนมัติ (Auto-Zero) ทันทีที่อ่านค่าเซ็นเซอร์ครั้งแรกสำเร็จ
  if (!autoZeroDone && (gotNewSample || hasC7Mpu) && millis() > 1500) {
    calibrateNeutralPosture();
  }

  // กดปุ่ม BOOT หรือส่ง 'z' ทาง Serial เพื่อตั้งศูนย์ใหม่เมื่อไหร่ก็ได้
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    calibrateNeutralPosture();
    delay(250);
  }
  while (Serial.available() > 0) {
    char cmd = (char)Serial.read();
    if (cmd == 'z' || cmd == 'Z' || cmd == 'c' || cmd == 'C') {
      calibrateNeutralPosture();
    }
  }

  // คำนวณมุมสัมพัทธ์จากจุดตั้งศูนย์ และจำกัดขอบเขตสรีระจริง
  headPitch = clampDeg(wrapDeg(rawHeadPitch - offsetPitch), -25.0f, 45.0f);
  headRoll  = clampDeg(wrapDeg(rawHeadRoll  - offsetRoll),  -30.0f, 30.0f);
  headYaw   = clampDeg(wrapDeg(rawHeadYaw   - offsetYaw),   -40.0f, 40.0f);

  if (hasC7Mpu) {
    float deltaC7 = wrapDeg(rawC7Angle - offsetC7);
    c7Angle = clampDeg(-9.0f + deltaC7 + (headPitch * 0.35f), -15.0f, 35.0f);
  } else {
    c7Angle = clampDeg(-9.0f + (headPitch * 0.65f), -15.0f, 35.0f);
  }

  cvaAngle = calculateCVA(c7Angle, headPitch);

  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();

    String jsonPayload = "{";
    jsonPayload += "\"c7_angle\":"   + String(c7Angle, 2) + ",";
    jsonPayload += "\"head_pitch\":" + String(headPitch, 2) + ",";
    jsonPayload += "\"head_roll\":"  + String(headRoll, 2) + ",";
    jsonPayload += "\"head_yaw\":"   + String(headYaw, 2) + ",";
    jsonPayload += "\"cva_angle\":"  + String(cvaAngle, 2);
    jsonPayload += "}";

    Serial.println(jsonPayload);
  }
}
