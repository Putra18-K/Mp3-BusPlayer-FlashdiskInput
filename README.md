# Bus MP3 Player — ESP32-S3 + USB Flash Drive + OLED

Pemutar MP3 mandiri untuk bus sekolah. Musik dicolok via **flashdisk USB (FAT32)** yang
dibaca USB-host native ESP32-S3. Library lagu ditampilkan di **OLED 0.91" (SSD1306, I2C)**,
audio diputar lewat **PCM5102A (I2S DAC)** → jack 3.5mm → preamp.
Kontrol: **1 tombol** (PLAY) — setiap tekan memutar lagu **berikutnya** secara berurutan.

Framework: **ESP-IDF 5.x / 6.x** (bukan Arduino) — baca flashdisk USB-host pakai jalur
resmi Espressif (`usb_host_msc`), dan open-source MP3 decoder **minimp3** dipakai untuk
memutar file tanpa ketergantungan framework audio yang rapuh. Flashdisk mendukung
**FAT32** dan **exFAT** (NTFS tidak didukung FatFS/ESP-IDF).

---

## Wiring (kabel custom)

| Ke mana            | Pin ESP32-S3 | Catatan |
|--------------------|--------------|---------|
| OLED SDA (SSD1306) | GPIO9        | I2C, alamat 0x3C, 128x32 |
| OLED SCL           | GPIO8        | 
| I2S BCK → PCM5102A | GPIO17       | 
| I2S WS (LRCK)      | GPIO15       | 
| I2S DOUT (DIN)     | GPIO16       | 
| I2S MCLK → SCK     | GPIO18       | 
| Tombol PLAY        | GPIO12       | 
| Tombol RESET       | GPIO11       | reset ke lagu pertama
| USB flashdisk D-   | GPIO19       | 
| USB flashdisk D+   | GPIO20       | 

**Semua pin bisa diubah**: macro ada di `main/oled.h`, `main/audio.h`, `main/app_main.c`.

### Catatan USB host
- Flashdisk **HARUS FAT32 atau exFAT** (bukan NTFS).
- **5V** ke flashdisk harus dari sumber 5V (rail/buck), **bukan** 3.3V.
- Jangan pakai port USB native untuk flashing bersamaan — GPIO19/20 dipakai untuk flashdisk.
  Flashing lewat port **USB-UART terpisah**, atau cabut flashdisk saat flash.
- Daya bus 5V harus mampu mensuplai flashdisk (jika flashdisk berat arusnya, beri power
  terpisah dan isolasi VBUS).
- exFAT memerlukan patch `FF_FS_EXFAT=1` di komponen FatFS ESP-IDF (`components/fatfs/src/ffconf.h`);
  jika ESP-IDF di-update/reinstall, patch tersebut perlu diulang.

### Catatan MCLK / PCM5102A
- Kalau GPIO6 → SCK PCM5102A tersambung: biarkan seperti di atas (S3 generate MCLK).
- Kalau **tidak** menyambung MCLK: set `PIN_I2S_MCLK` ke `I2S_GPIO_UNUSED` di `main/audio.h`
  **dan** bind SCK PCM5102A ke GND (mode auto). Bunyi/klik jika salah satu tidak konsisten.

---

## Sebelum build: download `minimp3.h`

minimp3 adalah decoder MP3 public-domain (satu file). File **tidak** saya commit. Letakkan di:

```
components/minimp3/include/minimp3.h
```

Unduh sekali (dari folder proyek):

```powershell
curl.exe -L -o components/minimp3/include/minimp3.h `
  https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h
```

CMake akan menolak build dengan pesan jelas jika file ini belum ada.

---

## Build & flash

### Opsi A — VSCode + extension ESP-IDF (disarankan)
1. Install [ESP-IDF VSCode Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
   dan ikuti wizard-nya (install ESP-IDF v5.x/6.x + ESP-IDF Tools).
2. `View > Command Palette > ESP-IDF: Open Folder` → pilih folder `bus-mp3-player`.
3. Klik tombol **Build** (🔧), lalu **Flash** + **Monitor** (serial 115200).

### Opsi B — VSCode + PlatformIO
Buat `platformio.ini` di root (belum disertakan):
```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
monitor_speed = 115200
```
Lalu Build/Upload via bilah PlatformIO. (PlatformIO juga menghormati
`main/idf_component.yml` untuk unduh dependensi terkelola.)

---

## Alur kerja (1 tombol)
- **Boot** → OLED menampilkan judul "MP3 BUS PLAYER" karakter demi karakter,
  lalu menahan judul lengkap selama ±3 detik.
- **Setelah judul** → OLED menampilkan "mencari usb".
- **Tanpa flashdisk** → tetap di layar "mencari usb".
- **Flashdisk dicolok** → OLED menampilkan "mencari file" dengan **loading bar
  beranimasi** selama playlist dipindai untuk file musik (`.mp3` dan `.wav`).
- **Setelah scan selesai** → OLED menampilkan "KLIK TOMBOL / UNTUK LANJUT".
- **Tekan tombol** → masuk mode player: **library 3 baris** — lagu sebelumnya,
  lagu aktif (tengah, disorot), lagu berikutnya. Tekan pertama memutar lagu 1.
- **Tekan lagi** → memutar lagu berikutnya, **membungkus** dari lagu terakhir
  kembali ke lagu 1.
- **Tekan saat lagu sedang berjalan** → **langsung skip** ke lagu berikutnya.
- Baris aktif menampilkan **progress bar** yang mengisi sesuai durasi lagu.
- Saat lagu habis → **berhenti** dan menunggu (tidak auto-play; tombol yang memajukan).
- **Flashdisk dicabut** → OLED kembali ke "mencari usb", audio berhenti.
- Flashdisk NTFS/exFAT yang tidak didukung → OLED menampilkan "flashdisk harus FAT32".

---

## Titik sensitif versi (cek saat build)
- Driver USB dipakai dari komponen terkelola **`usb_host_msc`** (`main/idf_component.yml`).
  API: `usb_host_install()`, `msc_host_install()`, `msc_host_install_device()`,
  `msc_host_vfs_register()`. Volume di-mount di **`/usb0`** (`USB_BASE_PATH` di
  `main/usb.h`). Jika nama API/komponen beda di versi terpasang, sesuaikan `main/usb.c`
  & `main/CMakeLists.txt` (`REQUIRES usb_host_msc`).
- Driver I2S memakai **API baru IDF 5.x/6.x** (`i2s_new_channel`,
  `i2s_channel_init_std_mode`, `i2s_channel_write`, `i2s_channel_reconfig_std_clock`).
  Di IDF 6.x header-nya butuh `esp_driver_i2c` + `esp_driver_i2s` di `REQUIRES`.
- Driver I2C OLED memakai API baru (`i2c_new_master_bus`, `i2c_master_transmit`).
- Decoder minimp3: makro `MINIMP3_MAX_SAMPLES_PER_FRAME`, field `info.hz` (bukan
  `sample_rate`).

---

## Verifikasi (di perangkat)
1. **Compile**: pastikan 0 error (CMake harus menemukan `minimp3.h`).
2. **Tanpa flashdisk**: OLED tampil "menunggu usb", tombol tidak menggantung.
3. **Flashdisk FAT32 berisi 2–3 file `.mp3`/`.wav`**: saat dicolok tampil "mencari",
   lalu library tampil (2 baris, pertama disorot); satu tekan tombol → lagu 1, tekan lagi
   → lagu 2, dan seterusnya.
4. **Wrap**: setelah lagu terakhir, tekan lagi → kembali ke lagu 1.
5. **Skip**: tekan saat sedang putar → langsung pindah lagu berikutnya.
6. **Sambung–putus flashdisk**: deteksi mount/remove; audio berhenti saat dicabut.
7. **Lagu > 5 menit**: pastikan streaming per-chunk stabil (tidak patah/stuck).

## Batasan yang diketahui
- File musik yang didukung: **MP3** (lewat minimp3) dan **WAV** (PCM 8/16-bit, mono/stereo,
  lewat parser RIFF sendiri). Format lain (flac/ogg/aac) tidak dideteksi.
- Nama file dengan karakter non-ASCII tampil sebagai spasi di OLED (font 5x7 ASCII).
  Sebaiknya beri nama file ASCII (`01-lagu.mp3`).
- Scan hanya folder **root** flashdisk (tidak rekursif ke subfolder).

---

Sumber referensi:
- [esp-usb-host / usb_host_msc](https://github.com/espressif/esp-usb-host)
- [minimp3](https://github.com/lieff/minimp3)
- [ESP-IDF I2S](https://docs.espressif.com/projects/esp-idf)
