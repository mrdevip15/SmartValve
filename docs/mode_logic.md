# Logika Mode & Syarat Trigger Servo

## Konstanta Referensi

| Konstanta | Nilai | Keterangan |
|---|---|---|
| `SOUND_THRESHOLD_DB` | 75.0 dB | Batas suara dianggap "keras" |
| `RPM_THRESHOLD` | 4000 RPM | Batas putaran dianggap "tinggi" |
| `MODE1_CLOSE_HOLD` | 1000 ms | Durasi hold close setelah suara hilang (IDLE) |
| `CYCLE_DURATION` | 60000 ms | Durasi satu fase siklus buka/tutup |

---

## MODE 1 — IDLE

**Filosofi**: Katup bereaksi langsung terhadap suara keras. Tidak peduli RPM.

### Syarat CLOSE
```
currentDB > 75.0 dB
```

### Syarat OPEN (kembali buka)
```
currentDB <= 75.0 dB  DAN  sudah diam >= 1000 ms
```

### Alur Detail
```
Suara keras terdeteksi
  → servoClose() segera
  → mode1CloseStart = sekarang  (timer terus diperbarui selama suara masih keras)

Suara hilang
  → tunggu 1000 ms sejak suara terakhir
  → servoOpen()
  → otomatis pindah ke MODE_NORMAL
```

> **Catatan**: `isCycleActive` tidak digunakan di mode ini. Mekanisme open/close
> dikendalikan murni oleh `mode1HoldActive` + timer `mode1CloseStart`.
> Transisi ke MODE_NORMAL hanya terjadi jika suara sempat trigger — bukan saat
> pertama kali masuk IDLE tanpa event apapun.

---

## MODE 2 — NORMAL

**Filosofi**: Katup menutup hanya jika **dua kondisi terpenuhi sekaligus** — suara keras
DAN mesin berputar cepat. Satu kondisi saja tidak cukup.

### Syarat CLOSE
```
currentDB > 75.0 dB  DAN  currentRPM > 4000
```

### Syarat OPEN (kembali buka)
```
salah satu kondisi tidak terpenuhi  (suara < 75 dB  ATAU  RPM <= 4000)
```

### Alur Detail
```
Kedua trigger aktif & siklus belum jalan
  → isCycleActive = true
  → servoClose()
  → cycleStartTime = sekarang

Salah satu trigger mati
  → resetCycle() → servoOpen()

Siklus aktif & sudah 60 detik
  → toggle fase: tutup ↔ buka
  → cycleStartTime diperbarui
```

---

## MODE 3 — BURU

**Filosofi**: Logika **terbalik** dari NORMAL. Katup menutup ketika mesin **lambat/berhenti**
(RPM rendah), buka ketika mesin kencang. Dipakai untuk skenario "berburu" / kondisi khusus.

### Syarat CLOSE
```
currentRPM <= 4000  (RPM RENDAH atau nol)
```

### Syarat OPEN (kembali buka)
```
currentRPM > 4000  (RPM TINGGI)
```

> **Perhatikan**: Mode ini **tidak memakai sensor suara** sama sekali sebagai trigger.
> Suara diabaikan.

### Alur Detail
```
RPM rendah & siklus belum jalan
  → isCycleActive = true
  → servoClose()
  → cycleStartTime = sekarang

RPM naik > 4000
  → resetCycle() → servoOpen()

Siklus aktif & sudah 60 detik
  → toggle fase: tutup ↔ buka
  → cycleStartTime diperbarui
```

---

## Perbandingan Cepat

| | MODE IDLE | MODE NORMAL | MODE BURU |
|---|---|---|---|
| Trigger utama | Suara | Suara **+** RPM | RPM rendah |
| Pakai sensor suara? | ✅ | ✅ | ❌ |
| Pakai sensor RPM? | ❌ | ✅ | ✅ |
| Logika RPM | — | Tinggi → close | **Rendah → close** |
| Pakai siklus 60 detik? | ❌ | ✅ | ✅ |
| Hold timer setelah trigger? | ✅ (1000 ms) | ❌ | ❌ |

---

## Mekanisme Siklus 60 Detik (MODE 2 & 3)

Berlaku di MODE_NORMAL dan MODE_BURU setelah trigger pertama aktif:

```
t=0       → trigger aktif → servoClose(), isClosingPhase=true
t=60s     → toggle → servoOpen(), isClosingPhase=false
t=120s    → toggle → servoClose(), isClosingPhase=true
...dst selama trigger masih aktif
```

Siklus **langsung berhenti** (tanpa menunggu 60 detik) jika trigger hilang.
