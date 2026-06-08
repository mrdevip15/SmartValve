# Pembelajaran Kalibrasi Sensor Suara (KY-037 vs MAX4466)

Dokumen ini mencatat temuan teknis penting saat melakukan kalibrasi sensor suara untuk sistem Smart Valve.

## 1. Perbedaan Hardware Utama

| Fitur | KY-037 (Sensor Murah) | MAX4466 (Pre-amp Presisi) |
|-------|-----------------------|--------------------------|
| **Titik Tengah (DC Offset)** | Manual (Harus putar baut kuning) | Otomatis (VCC / 2) |
| **Output** | Sinyal kasar, noise tinggi | Sinyal halus, noise rendah |
| **Sensitivitas** | Rendah (butuh suara keras) | Tinggi (bisa dengar bisikan) |
| **Konfigurasi** | Membingungkan untuk pemula | Konsisten & Stabil |

---

## 2. Masalah "Stuck 1023" (Saturasi ADC)

**Pelajaran:** Jangan menggunakan `analogReference(INTERNAL)` jika tegangan diam (offset) sensor lebih tinggi dari 1.1V.

- **Kronologi:** MAX4466 diberi power 3.3V, maka titik diamnya adalah **1.65V**.
- **Kesalahan:** Saat kode menggunakan referensi 1.1V, tegangan 1.65V dianggap "melebihi batas langit-langit", sehingga ADC Arduino selalu membaca **1023**.
- **Solusi:** Gunakan `analogReference(DEFAULT)` (5V) agar Arduino memiliki "ruang pandang" yang cukup untuk melihat sinyal 1.65V tersebut di titik ~340.

---

## 3. Pentingnya Power 3.3V

**Pelajaran:** Pin 5V pada Arduino seringkali "kotor" karena berbagi jalur dengan Servo, Motor, atau USB.
- MAX4466 sangat sensitif terhadap noise listrik.
- Menggunakan pin **3.3V** memberikan daya yang lebih bersih, sehingga pembacaan dB di LCD tidak melompat-lompat liar (*jitter*).

---

## 4. Logika Pengolahan Sinyal

Untuk mendapatkan nilai dB yang akurat dari sensor analog, digunakan dua tahap:

### A. Windowing (Peak-to-Peak)
Karena suara adalah gelombang (sinus), kita tidak bisa mengambil satu sampel acak. Kita harus mencari selisih antara **Nilai Tertinggi (Max)** dan **Nilai Terendah (Min)** dalam jendela waktu tertentu (misal 150ms).
- **Rumus:** `P2P = Max - Min`

### B. Moving Average (Smoothing)
Untuk mencegah angka di LCD berubah terlalu cepat, digunakan rata-rata dari beberapa data P2P terakhir.
- **Optimasi:**
    - **16 Data:** Sangat stabil, tapi lambat bereaksi (jeda ~2.4 detik).
    - **4 Data:** Sangat responsif (jeda ~0.6 detik), cocok untuk sistem katup yang butuh reaksi cepat.

---

## 5. Rumus Kalibrasi Logaritmik (dB)

Suara diukur dalam skala logaritmik, bukan linear.
```cpp
float db = DB_REF + DB_SCALE * log10f(current_P2P / P2P_REF);
```
- **DB_REF:** Angka dasar (misal 42.0 dB saat sunyi).
- **P2P_REF:** Nilai P2P saat sunyi (sebagai pembanding).
- **DB_SCALE:** Faktor pengali untuk menentukan seberapa jauh angka naik saat suara keras.

---

## Tips Debugging Masa Depan
1. Selalu cek nilai **Raw ADC** (Min & Max) sebelum menghitung dB.
2. Gunakan Multimeter untuk cek voltase di pin **AO**. Jika > 1.1V, jangan pakai INTERNAL ref.
3. Pastikan **Grounding** kuat (hambatan < 1 Ohm) untuk menghindari induksi tubuh saat sensor disentuh.
