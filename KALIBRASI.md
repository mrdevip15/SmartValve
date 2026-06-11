# Panduan Kalibrasi Sensor

## Bagian A — Kalibrasi Sensor Suara KY-037

### 1. Kalibrasi Fisik (Gain Potentiometer)
Putar potensiometer biru pada sensor:
- Putar **searah jarum jam (CW)** → Sensitivitas **turun**.
- Putar **berlawanan jam (CCW)** → Sensitivitas **naik**.

**Target:**
- Saat sunyi: P2P di LCD sekitar 20-80.
- Saat suara keras target: P2P di LCD sekitar 400-700.

### 2. Kalibrasi Software (main.cpp)
Update nilai berikut setelah testing:
```cpp
#define DB_REF 36.5f   // Ganti dengan angka dB dari Sound Meter HP saat sunyi
#define P2P_REF 16.0f  // Ganti dengan angka P2P di LCD saat sunyi
```

---

## Bagian B — Kalibrasi Sensor RPM (FC-03 Speed Sensor)

Sensor ini bekerja dengan mendeteksi hambatan (slot) pada piringan encoder yang berputar.

### Step 1 — Cek Fisik & Alignment
1.  Pastikan piringan encoder masuk tepat di tengah celah sensor FC-03.
2.  Putar mesin/shaft secara manual dan sangat pelan.
3.  Lihat LCD atau Serial Monitor bagian **IR**:
    *   **IR:H** (HIGH) → Saat sensor berada di lubang/celah piringan.
    *   **IR:L** (LOW) → Saat sensor terhalang bodi piringan.
4.  Jika status tidak berubah (stuck di H atau L), geser posisi sensor agar lebih presisi.

### Step 2 — Menentukan Nilai PPR (Pulses Per Revolution)
PPR adalah jumlah pulsa yang dihasilkan dalam 1 putaran penuh.
1.  Hitung jumlah lubang/celah pada piringan encoder Anda.
    *   Jika pakai piringan bawaan FC-03 biasanya **20 lubang** (PPR = 20).
    *   Jika hanya pakai satu baling-baling/baut (PPR = 1).
2.  Update nilai ini di `src/main.cpp` pada baris:
    ```cpp
    #define RPM_PPR 20  // Ganti sesuai jumlah lubang piringan Anda
    ```

### Step 3 — Verifikasi Akurasi
1.  Putar shaft tepat **1 putaran penuh** menggunakan tangan.
2.  Lihat nilai **P** (Pulses) pada LCD:
    *   Jika piringan 20 lubang, nilai **P** harus menunjukkan angka **20**.
    *   Jika nilai **P** melompat jauh (misal jadi 40-50), berarti ada **noise listrik**. Pastikan kabel sensor jauh dari kabel koil/busi.

### Step 4 — Menentukan Threshold Trigger
1.  Nyalakan mesin.
2.  Cari kecepatan (RPM) di mana Anda ingin katup mulai bekerja (menutup).
3.  Lihat angka **RPM** yang muncul di LCD.
4.  Update nilai threshold di `src/main.cpp`:
    ```cpp
    #define RPM_THRESHOLD 4000 // Ganti dengan RPM target Anda
    ```

---

## Tips Anti-Noise (Penting untuk RPM)
Jika RPM melompat-lompat tidak masuk akal saat mesin nyala:
1.  Gunakan kabel **Shielded** (kabel isi 3 yang dibungkus ground) untuk sensor RPM.
2.  Pasang kapasitor **0.1uF (kode 104)** antara pin GND dan pin Signal di sensor untuk memfilter percikan api busi.
3.  Pastikan Arduino mendapatkan power yang stabil (lewat step-down, jangan langsung dari aki tanpa filter).
