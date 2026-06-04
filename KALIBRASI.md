# Panduan Kalibrasi Sensor Suara KY-037

## Alat yang Dibutuhkan

| Alat | Keterangan |
|------|-----------|
| HP A | Instal app **Tone Generator** (Play Store / App Store) |
| HP B | Instal app **Sound Meter** (Play Store / App Store) |
| Arduino Uno | Sudah upload code terbaru, nyala pakai adaptor |
| Obeng kecil (+) | Untuk putar potensiometer di sensor KY-037 (jika perlu) |

---

## Bagian A — Kalibrasi Fisik Sensor KY-037

KY-037 punya dua output:
- **DO** (Digital Out) → hanya HIGH/LOW, tidak dipakai di sistem ini
- **AO** (Analog Out) → tegangan proporsional dengan suara, **ini yang dipakai**

### Komponen di board KY-037

```
┌─────────────────────────┐
│  [LED power]            │
│  [LED sinyal DO]        │
│                         │
│   ┌──────────┐          │
│   │ Mic      │          │
│   └──────────┘          │
│                         │
│   [Potensiometer] ←─── ini yang diputar untuk kalibrasi sensitifitas
│                         │
│  AO  DO  GND  VCC       │
└─────────────────────────┘
```

### Cara atur potensiometer

Potensiometer = sekrup kecil di tengah board. Fungsinya: atur **gain / sensitifitas** mikrofon.

- Putar **searah jarum jam (CW)** → sensitifitas **turun** → P2P lebih kecil
- Putar **berlawanan jarum jam (CCW)** → sensitifitas **naik** → P2P lebih besar

### Kapan perlu putar potensiometer?

| Kondisi | Tindakan |
|---------|----------|
| P2P saat sunyi sudah >200 (terlalu sensitif) | Putar CW sedikit |
| P2P saat noise keras <100 (kurang sensitif) | Putar CCW sedikit |
| P2P saat noise keras >900 (hampir saturasi) | Putar CW sedikit |
| P2P saat noise keras 300–700 (ideal) | Tidak perlu diubah |

**Target P2P saat sunyi:** 20–80  
**Target P2P saat noise referensi:** 300–700

---

## Bagian B — Kalibrasi Software (DB_REF & V_REF)

### Step 1 — Posisi alat

```
         5–10 cm          5–10 cm
HP A ◄──────────► KY-037 ◄──────────► HP B
(noise)           (sensor)           (sound meter)
```

- HP A dan HP B sejajar dengan sensor
- Semua di posisi tetap, jangan dipegang saat baca nilai

---

### Step 2 — Putar noise

1. Buka app **Tone Generator** di HP A
2. Pilih **Sine Wave, frekuensi 1000 Hz**
3. Naikkan volume HP A perlahan
4. Lihat HP B (Sound Meter) sampai angka stabil di nilai yang kamu inginkan  
   Rekomendasi: **70–80 dB** (lebih mudah dicapai dari 90 dB)
5. **Jangan geser posisi apapun setelah ini**

---

### Step 3 — Catat nilai P2P dari LCD

1. Lihat LCD Arduino baris bawah → tampil `P2P:XXX`
2. Tunggu **10 detik** sampai angka relatif stabil
3. Catat nilai tengah-tengahnya (bukan nilai tertinggi/terendah)

Contoh: LCD menampilkan `P2P:480`

---

### Step 4 — Hitung V_REF

```
V_REF = (nilai_P2P ÷ 1023) × 5.0
```

**Contoh:**
```
V_REF = (480 ÷ 1023) × 5.0 = 2.35
```

Gunakan kalkulator HP untuk hasil akurat.

---

### Step 5 — Update code

Buka file `src/main.cpp`, cari bagian ini (sekitar baris 48):

```cpp
#define DB_REF 60.0f
#define V_REF  2.5f
```

Ganti dengan nilai hasil kalibrasi:

```cpp
#define DB_REF 75.0f   // ← isi dB yang terbaca di HP B (Sound Meter)
#define V_REF  2.35f   // ← hasil hitungan Step 4
```

---

### Step 6 — Upload & verifikasi

1. Upload ulang code ke Arduino
2. Nyalakan pakai adaptor
3. Cek hasil:

| Kondisi | Pembacaan LCD Diharapkan |
|---------|--------------------------|
| Ruangan sunyi | 30–50 dB |
| Percakapan normal (60 dB) | 55–65 dB |
| Noise referensi (misal 75 dB) | 70–80 dB |
| Suara keras (85+ dB) | 80–90 dB |

Toleransi wajar: **±5 dB**

---

### Step 7 — Ulangi jika masih meleset

Jika hasil meleset >10 dB dari ekspektasi:

1. Pastikan posisi HP dan sensor sama persis seperti Step 1
2. Ulangi Step 2–4 dengan P2P yang lebih akurat
3. Jika P2P saat noise masih kecil (<150) → putar potensiometer CCW lalu ulangi

---

## Setelah Kalibrasi Selesai

Hapus tampilan P2P dari LCD (opsional, untuk tampilan lebih rapi):

Buka `src/main.cpp`, cari bagian `updateLCD()`, ganti baris:
```cpp
snprintf(lcdLine1, sizeof(lcdLine1), "P2P:%-4u %sdB", lastPeakToPeak, dbBuf);
```
Menjadi:
```cpp
snprintf(lcdLine1, sizeof(lcdLine1), "%sdB%s", dbBuf, closed ? " CLOSE" : " OPEN ");
```

Upload ulang.

---

## Catatan Penting

- KY-037 **tidak punya kalibrasi factory** — akurasi ±5 dB sudah sangat baik untuk sensor ini
- Jauhkan sensor dari sumber getaran mekanik (fan, motor) saat kalibrasi
- Kalibrasi ulang jika sensor dipindah lokasi atau gain potensiometer diubah
- Threshold sistem saat ini: **75 dB** → servo menutup katup (bisa diubah di `#define SOUND_THRESHOLD_DB`)
