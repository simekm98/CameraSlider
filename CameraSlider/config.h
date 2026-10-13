#pragma once

// ============================================================================
//  Piny – ESP32-S3-N16R8
//  NEPOUŽÍVAT: GPIO 26–32 (flash), 33–37 (octal PSRAM), 19/20 (USB),
//              0/3/45/46 (strapping), 43/44 (UART0)
// ============================================================================
#define PIN_STEP       4
#define PIN_DIR        5
#define PIN_EN         6    // aktivní v LOW
#define PIN_TMC_TX     17   // ESP TX -> 1 kΩ -> PDN_UART
#define PIN_TMC_RX     18   // ESP RX -> přímo PDN_UART
#define PIN_HALL       10   // Hallovo čidlo (open-collector, interní pull-up)
#define PIN_RESET_JMP  11   // jumper na GND = normál, rozpojeno = reset nastavení
#define PIN_CAM_SHUTTER 12  // drátová spoušť: -> 330 Ω -> optočlen PC817 -> hrot jacku E3
#define PIN_CAM_FOCUS   13  // drátové ostření: -> 330 Ω -> optočlen PC817 -> kroužek jacku E3

// ============================================================================
//  TMC2209
// ============================================================================
#define TMC_R_SENSE    0.11f   // většina modulů (BTT, FYSETC) má 0.11 Ω
#define TMC_ADDRESS    0b00    // MS1 = MS2 = GND
#define TMC_BAUD       115200

// ============================================================================
//  Síť
// ============================================================================
#define AP_SSID        "CameraSlider"
#define AP_PASS        "CameraSlider78963"
#define HOSTNAME       "CameraSlider"
#define MDNS_NAME      "cameraslider"     // -> http://cameraslider.local

#define STA_CONNECT_TIMEOUT_MS  20000     // jak dlouho zkoušet připojení k WiFi
#define STA_RETRY_MS            60000     // po neúspěchu zkusit znovu za...

// ============================================================================
//  Pohyb
// ============================================================================
#define MAX_STEP_HZ         150000   // strop frekvence kroků
#define HOME_ACCEL_MMS2     200.0f   // brzdění při homingu (malý přejezd čidla)
#define JOG_TIMEOUT_MS      600      // ruční posuv se zastaví, když UI přestane posílat keepalive
#define STATUS_INTERVAL_MS  100      // jak často posílat stav do UI
