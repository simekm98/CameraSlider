# CameraSlider – spoušť fotoaparátu (doplněk)

## Co je nového
- **Bluetooth spoušť Canon** – ESP32-S3 se tváří jako dálkový ovladač Canon BR-E1 (knihovna NimBLE-Arduino).
- **Drátová spoušť** (volitelně) – GPIO 12 = spoušť, GPIO 13 = ostření, každý přes 330 Ω na optočlen PC817 → jack 2,5 mm (E3): hrot = spoušť, kroužek = ostření, návlek = zem.
- **Focení podle polohy** – během programu (cyklus / 1 cyklus) snímek každých *X mm*, nezávisle na rozjezdu a brzdění. Volitelně jen v jednom směru.
- **Čekání na tlačítko Continue** na koncích (otočení desky) místo časovače.
- **Záznam snímků** pro třídění fotek v PC: `http://192.168.4.1/shots.csv` a `http://192.168.4.1/api/shots` (JSON). Každý průjezd s focením = jeden „pass“ (= jedna plocha desky). Záznam se maže při každém startu programu.
- Kontrola před startem: program nespustí, pokud by snímky šly rychleji než *Min. interval*, nebo když fotoaparát není připojen.
- Původní nastavení pojezdu (kalibrace, WiFi) zůstává – kamera má vlastní klíč v NVS.

Nové soubory: `camera.h/.cpp`, `capture.h/.cpp`. Upravené: `CameraSlider.ino`, `config.h`, `motion.h/.cpp`, `net.cpp`, `data/index.html`, `data/app.js`, `data/style.css`.

## Nahrání (Arduino IDE)
1. **Záloha současného firmwaru** (doporučeno). V příkazové řádce (COMx = port ESP32):
   ```
   "%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\<verze>\esptool.exe" --chip esp32s3 --port COMx read_flash 0 0x1000000 zaloha_cameraslider.bin
   ```
   Návrat k záloze: `esptool ... write_flash 0 zaloha_cameraslider.bin`
2. Library Manager → nainstalovat **NimBLE-Arduino** (h2zero, verze 2.x).
3. Nastavení desky stejné jako dosud (ESP32S3 Dev Module, Flash 16 MB, PSRAM OPI, Partition Scheme: custom / partitions.csv). **Erase All Flash: Disabled** (jinak se smaže nastavení).
4. Upload sketche.
5. **Nahrát i web UI**: Ctrl+Shift+P → *Upload LittleFS to Pico/ESP8266/ESP32* (složka `data`). Bez toho se nové ovládání kamery v prohlížeči neobjeví.

Ověřeno překladem: ESP32 Arduino core 3.3.12, NimBLE-Arduino 2.5.1, ESPAsyncWebServer 3.12.1, FastAccelStepper 2.0.0.

## Párování Canon 77D
1. Fotoaparát: *Nastavení bezdrátové komunikace → Funkce Bluetooth → Dálkové ovládání*, pak *Párování*.
2. Web vozíku → sekce **Camera** → Trigger = Bluetooth → **Pair camera** (do 20 s).
3. **Test shot** → fotoaparát musí vyfotit.
4. Na fotoaparátu vypnout automatické vypnutí; režim závěrky dálkové ovládání / samospoušť dle manuálu 77D.

## Sken desky (4 plochy)
- Automatic mode: Count = **2 cykly** (1 cyklus = Od→Do + Do→Od = 2 průjezdy).
- Camera: zaškrtnout *Take photos* a *wait for Continue*, Spacing např. 100 mm.
- Start cycling → po každém průjezdu otočit desku → **Continue**.
- Po skenu stáhnout `shots.csv`.

## Neověřeno na hardwaru
BLE protokol převzat z ověřených projektů (testováno na Canon M50, 77D používá stejný ovladač BR-E1), ale na tomto zařízení ještě neběžel. Při potížích poslat výpis ze Serial monitoru (115200, řádky `[cam]`).
