# ImgTxd Hook — AML plugin untuk GTA SA 2.10 (64-bit)

`libimgtxd_hook.so` adalah mod [AndroidModLoader](https://github.com/RusJJ/AndroidModLoader)
(GUID `net.imgtxd.hook`) yang menjembatani ImgTxd Studio ke GTA SA 2.10
yang sedang berjalan:

```
ImgTxd Studio ──IPC (TCP loopback 127.0.0.1:38177)──► libimgtxd_hook.so (di proses game)
                                                       ├─ hook ConvertBufferToObject → substitusi buffer DFF/TXD per modelId
                                                       └─ hook OS_ScreenSwapBuffers  → drain antrean RELOAD + job GL di render thread
```

Detail format kabel: [docs/PROTOCOL.md](docs/PROTOCOL.md).

**Kenapa TCP loopback (protokol v1.1):** v1.0 memakai socket Unix abstrak
+ FD lewat SCM_RIGHTS. Uji di HP user (2026-10-23, server tiruan di
Termux) membuktikan koneksi socket abstrak lintas aplikasi **diblokir
isolasi SELinux per-aplikasi** — klien tidak pernah tiba di listener.
TCP loopback tidak terkena batasan itu dan APK game 2.10 punya izin
INTERNET, jadi transport diganti; header dan logika substitusi tetap.

## Build

```sh
./build.sh   # memakai NDK di ~/workspace/android-ndk (aarch64-linux-android26-clang++)
# hasil: build/libimgtxd_hook.so  (ELF64 AArch64)
```

SDK AML di-vendor dari repo resmi RusJJ/AndroidModLoader ke `vendor/aml-sdk`
(template `template_of_mod/mod/` disalin ke `src/mod/`, sesuai cara pakai
SDK). STL di-link statis (`-static-libstdc++`, seperti `APP_STL := c++_static`
di template), jadi `DT_NEEDED` hanya `liblog, libdl, libm, libc`.
Permukaan ekspor dibatasi version script ke entry AML: `__GetModInfo`,
`OnModLoad`.

## Resolusi simbol game

Semua alamat game diresolusi **saat runtime** dari nama simbol mangled —
bukan VA mentah — dengan urutan:

1. `aml->GetSym(base libGTASA.so, nama)` (utama),
2. `dlsym` pada handle `libGTASA.so` (cadangan),
3. VA hardcoded build acuan (**fallback terakhir**, hanya dipakai bila
   panjang `libGTASA.so` yang termuat persis 54.731.968 byte dan selalu
   dicatat sebagai `VA-FALLBACK` di log).

| Simbol (mangled) | Dipakai untuk |
|---|---|
| `_ZN10CStreaming21ConvertBufferToObjectEPcii` | hook substitusi buffer |
| `_Z20OS_ScreenSwapBuffersv` | hook titik drain render thread |
| `_ZN10CStreaming12RequestModelEii` | trigger RELOAD |
| `_ZN10CStreaming11RemoveModelEi` | trigger RELOAD |
| `_ZN10CStreaming16ms_aInfoForModelE` | array CStreamingInfo (stride 20, CdSize @ +0x0C) |
| `_ZN6CPools15ms_pVehiclePoolE` | diresolusi & dicatat (iterasi pool = v2) |
| `_Z25RwTexDictionaryGetCurrentv` | kesiapan TEXUPDATE (lihat TODO) |
| `_Z31RwTexDictionaryFindNamedTextureP15RwTexDictionaryPKc` | kesiapan TEXUPDATE (lihat TODO) |

Setiap resolusi (sukses/gagal + caranya) ditulis ke logcat tag
**`ImgTxdHook`** saat `OnModLoad`.

## Pasang di HP

Sesuai dokumentasi AML, salin `libimgtxd_hook.so` ke folder mods game:

- Eksternal: `/sdcard/Android/data/com.rockstargames.gtasa/mods/`, atau
- Internal (default AML): `/sdcard/AMLMods/com.rockstargames.gtasa/`

Lalu jalankan game seperti biasa (AML memuat mod saat startup).

## Urutan uji di HP

1. **Pasang & boot.** Jalankan game, saring logcat: `adb logcat -s ImgTxdHook`.
   Harus muncul banner `ImgTxdHook v1.0 loading`, base + panjang
   libGTASA.so, satu baris `sym ... -> OK` per simbol, dua baris
   `hook ...: OK`, dan `IPC: listening on tcp 127.0.0.1:38177`.
2. **PING.** Dari aplikasi pendamping (atau klien uji), kirim header PING.
   Lolos bila menerima balasan 1 byte `0x01` dan log menulis
   `IPC: PING -> pong`. Bukti juga dari sisi game: job self-test
   `GL job (render thread): ImgTxdHook GL job queue alive` muncul sekali
   di awal (jalur render thread hidup).
3. **SUBSTITUTE satu model.** Kirim DFF/TXD sebuah model (mis. kendaraan)
   via cmd SUBSTITUTE (payload di-stream setelah header) untuk
   modelId-nya. Log harus menulis
   `IPC SUBSTITUTE model <id>: <n> bytes buffered`.
4. **RELOAD.** Kirim cmd RELOAD untuk modelId yang sama. Pada frame
   berikutnya log menulis `RELOAD: RemoveModel(<id>) + RequestModel(<id>,
   0x14) on render thread`, disusul saat model di-stream ulang:
   `CBO: model <id> substitute <n> bytes (CdSize a -> b)` dan
   `CdSize restored`. Model tampil dengan data baru di game.

## Batasan v1 / TODO

- **Refresh instance yang sudah spawn:** RELOAD men-stream ulang model,
  tetapi kendaraan/entitas yang sudah ada di dunia mungkin tetap memakai
  objek lama sampai respawn. Iterasi `CPools::ms_pVehiclePool` +
  `SetModelIndex` (simbol sudah terpetakan) adalah langkah v2.
- **TEXUPDATE (glTexSubImage2D ke tekstur bernama):** belum di-wire.
  Kedua simbol Rw sudah diresolusi dan antrean job GL (titik eksekusi
  render thread) sudah ada; yang kurang adalah perintah IPC baru
  (nama tekstur + piksel) di protokol. Lihat `docs/PROTOCOL.md`.
- **CdSize ditambal per panggilan:** substitusi mengandalkan layout
  `CStreamingInfo` stride 20 dari build 2.10 acuan; modelId di luar
  0..26315 otomatis dilewati ke fungsi asli tanpa substitusi.
- Plugin tidak pernah memanggil fungsi game dari thread listener; semua
  aksi game terjadi di hook (thread streaming / render thread).
