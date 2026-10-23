# ImgTxd IPC Protocol v1.1

Protokol antara aplikasi pendamping (ImgTxd Studio) dan plugin
`libimgtxd_hook.so` yang berjalan di dalam proses GTA SA 2.10 (AML mod).
Dokumen ini mendeskripsikan format persis seperti diimplementasikan di
`src/main.cpp`.

> **Perubahan v1.0 → v1.1 (transport saja):** v1.0 memakai socket Unix
> namespace abstrak dengan payload sebagai FD yang dilewatkan lewat
> `SCM_RIGHTS`. Transport itu **diganti total** setelah terbukti di HP
> user (2026-10-23): koneksi socket abstrak lintas aplikasi diblokir
> isolasi SELinux per-aplikasi — klien dari aplikasi lain tidak pernah
> tiba di listener. TCP loopback tidak terkena batasan itu, dan APK game
> 2.10 memiliki izin INTERNET. Header dan semantik perintah tidak
> berubah; yang berubah hanya cara payload SUBSTITUTE dikirim (kini
> di-stream, bukan FD).
>
> **Plugin v1.2 (protokol tetap v1.1):** hanya semantik RELOAD yang
> berubah — drain reload kini berpagar state + refcount agar tidak
> memaksa melepas model yang sedang dipakai (lihat seksi RELOAD).

## Transport

- Socket `AF_INET`, `SOCK_STREAM` (TCP).
- Server bind **hanya** ke `127.0.0.1` port **38177** (`SO_REUSEADDR`).
  Tidak ada listener di antarmuka lain.
- Server (listener) hidup di proses game; klien adalah aplikasi pendamping.
- **Satu koneksi = satu pesan.** Server menutup koneksi setelah memproses
  satu pesan. Batas waktu baca sisi server: 5 detik per koneksi
  (timeout juga berlaku saat membaca payload SUBSTITUTE).

### Catatan keamanan (jujur)

Listener loopback dapat disambung oleh **aplikasi lokal lain** di
perangkat yang sama — tidak ada autentikasi di v1.1. Ini diterima secara
sadar untuk v1 alat modding sideload yang dipakai di perangkat sendiri:
yang bisa dieksploitasi pihak lokal sebatas mengganti buffer model di
game yang sedang berjalan. Token/handshake bersama adalah pekerjaan
masa depan bila protokol ini perlu diperkeras.

## Header pesan (24 byte, little-endian)

| Offset | Tipe | Nama        | Keterangan |
|-------:|------|-------------|------------|
| 0x00   | u32  | magic       | Selalu `0x44585449` (`'ITXD'`). Selain itu pesan dibuang. |
| 0x04   | u16  | cmd         | 1 = SUBSTITUTE, 2 = RELOAD, 3 = PING |
| 0x06   | u16  | reserved    | Kirim 0. Diabaikan server. |
| 0x08   | i32  | modelId     | ID model streaming game (0..26315). |
| 0x0C   | u32  | pad         | Padding alignment. Kirim 0. |
| 0x10   | u64  | payloadSize | Hanya untuk SUBSTITUTE: ukuran payload (byte). |

## Perintah

### 1 — SUBSTITUTE

Klien mengirim header, lalu **payload (biner DFF/TXD mentah) di-stream
lewat koneksi yang sama** tepat setelah header, sebanyak tepat
`payloadSize` byte. Tidak ada FD, tidak ada data ancillary.

Perilaku server:

- `payloadSize == 0` atau `payloadSize > 512 MiB`: ditolak, dicatat di
  log, tidak ada state berubah.
- Server membaca payload sampai genap `payloadSize` byte. Baca pendek
  (timeout 5 detik / koneksi putus di tengah): pesan ditolak, dicatat
  di log, tidak ada state berubah.
- Payload yang utuh disimpan dalam buffer heap milik plugin sebagai
  substitusi *pending* untuk `modelId`, sampai terpakai atau diganti.
- Bila sudah ada pending untuk `modelId` yang sama, yang lama diganti
  (buffer lamanya dibebaskan).
- Saat game memanggil `CStreaming::ConvertBufferToObject` untuk
  `modelId` itu, plugin memakai buffer pending sebagai pengganti buffer
  game, menambal `CStreamingInfo.CdSize` menjadi `ceil(size / 2048)`
  selama pemanggilan, memulihkannya sesudahnya, lalu membebaskan
  pending.

### 2 — RELOAD

Header saja, tanpa payload. Server mengantre `modelId`. Pada
`OS_ScreenSwapBuffers` berikutnya (render thread), antrean dikuras dan
tiap modelId diproses lewat **pagar state + refcount** (perilaku
**plugin v1.2**; format kawat tidak berubah dari v1.1):

- Plugin membaca byte state dari `CStreamingInfo` model itu (offset
  `0x10`, terbukti dari disassembly `CStreaming::RemoveModel` di biner
  2.10) dan, bila relevan, refcount `CBaseModelInfo` (uint16 di offset
  `0x22`, field yang sama-sama dibaca `AddRef`/`RemoveRef` dan jalur
  purge game `RemoveLeastUsedModel`; pointer modelInfo diambil dari
  array game `CModelInfo::ms_modelInfoPtrs`). Catatan: offset `0x24`
  **bukan** refcount — itu `TxdIndex`.
- **state == LOADED (1) dan refcount > 0** (model sedang dipakai
  instance hidup): `RemoveModel`/`RequestModel` **dilewati**. Memaksa
  RemoveModel pada model LOADED menghancurkan RwObject di bawah
  instance yang masih mereferensikannya (fungsi itu sendiri tidak
  mengecek refcount) dan membuat game force close — itulah akar
  masalah yang diperbaiki v1.2. Substitusi pending **tetap terpasang**
  dan berlaku pada pemuatan alami berikutnya setelah model ter-purge.
- **state == LOADED (1) dan refcount == 0**: aman — plugin memanggil
  `CStreaming::RemoveModel(id)` lalu `CStreaming::RequestModel(id,
  0x14)`, persis semantik purge game.
- **state != LOADED**: hanya `CStreaming::RequestModel(id, 0x14)`.
- **state/refcount tidak terbaca** (simbol/state tak ter-resolve atau
  pointer modelInfo null): reload **dilewati** (gagal-aman; tidak ada
  fallback ke perilaku lama).

Setiap keputusan ditulis ke log `ImgTxdHook` dalam satu baris yang
memuat id, state, refcount, dan keputusan (`request-only` / `skip` /
`remove+request`).

Antrean job GL generik juga dikuras di titik yang sama; saat ini job
yang ada hanya tipe LOG (bukti jalur render thread hidup).

### 3 — PING

Header saja. Server membalas tepat **1 byte `0x01`** lalu menutup
koneksi. Tidak mengubah state apa pun. Ini uji hidup paling awal: bila
PING dibalas, listener dan jalur log pasti bekerja.

## Catatan klien

- Sambung ke `127.0.0.1:38177`, kirim header 24 byte; untuk SUBSTITUTE
  lanjutkan dengan payload persis `payloadSize` byte pada koneksi yang
  sama, lalu tutup/tunggu server menutup.
- Jangan mengharapkan balasan untuk SUBSTITUTE/RELOAD (hanya PING yang
  dibalas).
- Semua log server muncul di logcat dengan tag `ImgTxdHook`; baris
  startup listener berbunyi `IPC: listening on tcp 127.0.0.1:38177`.
