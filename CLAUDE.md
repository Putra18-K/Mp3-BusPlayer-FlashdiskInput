## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).

---

# Bus MP3 Player — ESP32-S3

## Ringkasan & tujuan

Pemutar MP3 mandiri untuk bus sekolah, berjalan di **ESP32-S3**. Musik dibaca dari
**flashdisk USB (FAT32)** lewat USB-host native (GPIO19/20), library ditampilkan di
**OLED 0.91" SSD1306 (I2C)**, audio diputar lewat **PCM5102A (I2S DAC)** → jack 3.5mm.
Kontrol dengan 3 tombol (UP / DOWN / SELECT). Semua logika ada di firmware — tidak ada
backend atau aplikasi host.

Jangan mengubah perilaku/API yang sudah berfungsi (playback, mount USB, render OLED)
tanpa kebutuhan nyata; verifikasi di perangkat sebelum menyimpulkan "aman".

## Teknologi & arsitektur

- **Framework:** ESP-IDF **5.x** (C murni, BUKAN Arduino / Arduino-ESP32 / ESP-ADF).
- **Target:** `esp32s3` (ditetapkan di `sdkconfig.defaults`).
- **Arsitektur:** multi-task FreeRTOS. `app_main` inisialisasi lalu membuat 4 task:
  `audio`, `buttons`, `ui`, `usb`. State bersama dilindungi mutex `s_mtx`
  (`main/app_main.c`). Audio berjalan pada task sendiri agar UI tetap responsif.
- **Komponen utama:**
  - `oled` — driver SSD1306 minimal (font 5x7), I2C master API baru IDF 5.x.
  - `audio` — decoder **minimp3** + streaming PCM 16-bit ke I2S std (PCM5102A).
  - `usb` — `esp_msc_host` (komponen terkelola `espressif/esp-iot-solution`)
    membaca flashdisk sebagai volume FatFS di `/usb`.
  - `minimp3` — decoder MP3 public-domain (satu file `.h` + satu TU `.c`).
- **Alur data:** flashdisk → `usb.c` scan `*.mp3` → `app_main.c` pilih lagu →
  `audio.c` decode & tulis ke I2S → DAC → jack.

## Struktur direktori penting

```
bus-mp3-player/
├── CMakeLists.txt              # proyek ESP-IDF (top-level), target esp32s3
├── sdkconfig.defaults          # default config target (jangan beri konfig lokal)
├── main/
│   ├── app_main.c              # entry point: init, playlist state, 4 task
│   ├── audio.c / audio.h       # I2S + minimp3 streaming, pin PCM5102A
│   ├── oled.c / oled.h         # driver SSD1306 + font, pin SDA/SCL
│   ├── usb.c / usb.h           # esp_msc_host, mount /usb, scan playlist
│   ├── CMakeLists.txt          # daftar SRCS + REQUIRES (termasuk esp_msc_host)
│   └── idf_component.yml       # dependensi terkelola (esp-iot-solution)
├── components/minimp3/
│   ├── CMakeLists.txt
│   ├── minimp3.c               # TU: #define MINIMP3_IMPLEMENTATION
│   └── include/minimp3.h       # TIDAK dicommit — unduh dulu (lihat README)
├── .claude/                    # integrasi Graphify (skill + hook + settings)
├── graphify-out/               # keluaran knowledge graph (jangan dicommit)
└── CLAUDE.md                   # file ini
```

Semua pin I/O dikendalikan makro di header: `main/oled.h` (`OLED_SDA_GPIO`,
`OLED_SCL_GPIO`, `OLED_WIDTH`, `OLED_HEIGHT`, `OLED_ADDR`), `main/audio.h`
(`PIN_I2S_BCLK/WS/DOUT/MCLK`), `main/app_main.c` (`PIN_BTN_UP/DOWN/SEL`).

## Aturan perubahan kode

1. **Jangan menurunkan fungsionalitas yang berjalan.** Build harus tetap 0 error;
   alur mount→scan→play→skip→stop→auto-advance jangan diputus.
2. Sebelum menyentuh hubungan antarkomponen, **periksa Graphify dulu**
   (`graphify query`/`path`/`explain`, lihat bagian Graphify di atas).
3. Setelah perubahan struktur/dependensi yang besar, **perbarui Graphify**
   dengan `graphify update .` (atau `/graphify .`).
4. Kode C murni — jangan tarik framework/Arduino/RTOS lain untuk hal yang sudah
   ditangani IDf + FreeRTOS.
5. Konfigurasi target via `sdkconfig.defaults`, bukan `menuconfig` yang tersimpan
   lokal. Jangan meng-hardcode konfigurasi pengguna di file source.
6. `minimp3.h` tidak boleh dimasukkan ke Git (tidak dicommit). Download command di
   README.md — CMake menolak build dengan pesan jelas jika file belum ada.
7. Uji di perangkat: perubahan audio/I2S wajib dicek suara; perubahan USB wajib
   dicek deteksi mount/remove flashdisk.

## Konvensi penamaan & gaya kode

- **Bahasa:** C99, gaya Espressif IDF. Komentar blok `/* ... */`; include header
  proyek dulu, lalu header IDF.
- **Fungsi:** `snake_case`. Fungsi di header di-export (tanpa prefix); fungsi
  internal `static` (tanpa prefix). Contoh: `audio_play()`, `set_sample_rate()`.
- **State bersama:** prefix `s_` untuk variabel `static` tingkat file
  (`s_names`, `s_mtx`, `s_playing`); `volatile` untuk flag lintas task
  (`s_cancel`, `s_playing` di `audio.c`).
- **Makro / konstanta:** `UPPER_SNAKE_CASE` (`MAX_TRACKS`, `DEBOUNCE_MS`,
  `PIN_I2S_BCLK`). Pin dibuat overridable via `#ifndef ... #define`.
- **Tag log:** `static const char *TAG = "<modul>"` per file, pakai `ESP_LOGI`/
  `ESP_LOGW`/`ESP_ERROR_CHECK`/`ESP_RETURN_ON_ERROR`.
- **Header guard:** `#pragma once`.
- **Task:** `xTaskCreate(fn, "nama", stackBytes, NULL, prio, NULL)`; nama task
  lowercase (`"audio"`, `"buttons"`, `"ui"`, `"usb"`).

## Perintah build, test, lint, run

Bangun/uji dilakukan lewat toolchain **ESP-IDF** (bukan make/gradle/npm). Aktifkan
lingkungan IDF dulu (biasanya `export.sh` / wizard VSCode ESP-IDF extension), pastikan
`IDF_PATH` terisi dan `minimp3.h` sudah diunduh.

```bash
# Build seluruh firmware (harus 0 error; CMake cek minimp3.h)
idf.py build

# Flash ke perangkat (ganti PORT, mis. COM3 / /dev/ttyACM0)
idf.py -p PORT flash

# Serial monitor 115200
idf.py -p PORT monitor

# Bersihkan artefak build
idf.py fullclean

# Konfigurasi menuconfig (hanya untuk dev; tetapkan default di sdkconfig.defaults)
idf.py menuconfig
```

Cara alternatif: **VSCode + extension ESP-IDF** (tombol Build/Flash/Monitor) atau
**PlatformIO** (framework `espidf`, lihat README).

**Test/lint:** tidak ada unit-test atau linter khusus yang terpasang untuk firmware.
"Verifikasi" = build 0 error + pengujian di perangkat (checklist di README.md:
tanpa flashdisk, playlist FAT32, deteksi mount/remove, lagu > 5 menit stabil).

## File sensitif (jangan dibuka, ditampilkan, atau dicommit)

Tidak boleh dibaca, ditampilkan, atau dimasukkan ke Git — baik di file, grafik,
maupun output percakapan:

- `.env` dan variasinya (`.env.*`)
- `local.properties`
- File kredensial, API key, token, kata sandi (mis. `*_key`, `*.pem`, `.credentials`)
- Konfigurasi lokal ter-generate: `sdkconfig` (bukan `sdkconfig.defaults`),
  `sdkconfig.old`, dsb.
- Direktori integrasi pribadi, file log, cache, artefak build.

Jika ditemukan file semacam itu saat bekerja, abaikan/jangan di-commit, dan laporkan
ke pengguna alih-alih memuat isinya.

## Integrasi Graphify

- Graphify sudah dipasang untuk proyek ini (`.claude/`, skill + PreToolUse hook).
- **Sebelum menganalisis hubungan antarkomponen**, jalankan `graphify query` /
  `graphify path` / `graphify explain` dulu (lihat bagian `## graphify` di atas).
- **Setelah perubahan struktur/dependensi besar**, perbarui graph dengan
  `graphify update .` atau `/graphify .`.
- Keluaran graph ada di `graphify-out/` (jangan dicommit). Saat menjawab pertanyaan
  tentang arsitektur/file/file relationship, prioritaskan query graphify di atas
  pencarian file mentah.
