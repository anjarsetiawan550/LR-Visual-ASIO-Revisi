# LR Visual ASIO

Driver ASIO virtual (WDM → ASIO) + GUI kontrol + installer EXE.

## Build (GitHub)
1. Upload seluruh isi folder ini ke repo GitHub (branch `main`).
2. Tab **Actions** → *Build LR Visual ASIO* → tunggu selesai.
3. Download artifact **LR_Visual_ASIO_Setup** → jalankan `LR_Visual_ASIO_Setup.exe` (Administrator).

## Pakai
- Buka **LR Visual ASIO** → pilih Output/Input device, Sample Rate (44.1–192 kHz), ASIO Buffer (32–2048), Safe Mode → **Terapkan**.
- Di DAW pilih driver ASIO: **LR Visual ASIO**.
- Panel **ASIO Status** menampilkan sample rate, latensi input/output, dan aktif/nonaktif (live dari driver).
- Ubah pengaturan saat DAW berjalan → driver meminta DAW reset otomatis.

## Cara kerja
- **Safe Mode OFF**: WASAPI Exclusive event-driven + MMCSS "Pro Audio" (latensi terendah; format otomatis 32/24/16-bit/float).
  Jika perangkat sedang dipakai/format tidak didukung → otomatis turun ke Shared.
- **Safe Mode ON**: WASAPI Shared + auto-konversi sample rate (paling kompatibel).
- Latensi nyata dibatasi periode minimum driver perangkat (sering ±2–3 ms); buffer di bawah itu tetap jalan lewat ring buffer.
- Angka latensi di GUI = estimasi (buffer ASIO + buffer perangkat).

## Latar belakang, tray & auto start
- Tombol ✕ menyembunyikan jendela ke **ikon tray** (klik panah "show hidden icons"); ikon berubah hijau saat driver AKTIF.
- Klik kanan ikon: Buka / Mulai otomatis dengan Windows / Keluar.
- Auto start aktif secara default (HKCU Run, argumen `--background`) dan tampil di **Task Manager → Startup apps** (bisa di-Disable di sana).

## Diagnosa
- Log driver: `%LOCALAPPDATA%\LRVisual\driver.log` (mode EXCLUSIVE/SHARED, format, error HRESULT, status input).
- Jika input tidak bersuara: cek Windows *Settings → Privacy → Microphone → Let desktop apps access your microphone*, aktifkan input monitoring/record arm di DAW, dan lihat meter LEVEL IN di aplikasi.

## Catatan
- Hanya **64-bit** (DAW 32-bit tidak akan melihat driver ini).
- Memakai WASAPI (jalur modern WDM audio), bukan WDM-KS mentah seperti ASIO4ALL.
- Antarmuka ASIO memakai header resmi **Steinberg ASIO SDK 2.3** (`driver/sdk/`: asio.h, asiosys.h, iasiodrv.h), sehingga layout struct (pack 4) dan selector sama persis dengan driver native.
- **Lisensi:** SDK berlisensi ganda. Proyek ini dirilis di bawah **GPLv3** (lihat `LICENSE`). Jika ingin rilis closed-source, tandatangani dulu Steinberg ASIO License Agreement. "ASIO" adalah merek dagang Steinberg.
- Ukuran buffer dipilih DAW (32-2048, kelipatan 2), sama dengan pilihan di aplikasi. Jika DAW mengganti buffer, nilainya disimpan ke registry dan chip buffer di aplikasi ikut berubah otomatis. Jika diubah dari aplikasi, DAW diminta reset dan driver menawarkan **satu ukuran saja** (yang baru) sampai DAW memakainya, lalu rentang penuh kembali. Sebagian DAW baru memuat ulang setelah Anda menutup/membuka ulang pengaturan audio.
- Jendela bisa di-resize (tarik tepi kanan, bawah, atau pojok kanan-bawah); isinya ikut diskalakan dan ukuran terakhir diingat.
- Logo **ASIO Compatible** (dari SDK, tidak diubah) ditampilkan di bawah ASIO Status, lengkap dengan atribusi merek dagang sesuai *Steinberg ASIO Usage Guidelines*.
- Kode ini belum dikompilasi/diuji di Windows oleh pembuatnya; jika Actions error, kirim log-nya untuk diperbaiki.
