# Semâ — Canlı Türkiye Hava Sahası İzleme

[![CI](https://github.com/mustafagulturk/sema-airspace-monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/mustafagulturk/sema-airspace-monitor/actions/workflows/ci.yml)

Semâ, Türkiye üzerindeki gerçek **sivil** uçak trafiğini [OpenSky Network](https://opensky-network.org)'ün
ücretsiz verisiyle canlı izleyen bir C++20 sistemidir. Simülasyon yoktur, her şey gerçek veriyle çalışır.

> **English summary.** Semâ is a C++20 system that tracks live civil air traffic over Turkey using
> OpenSky Network data. Phase 1 (this code) collects state vectors with an OAuth2, quota-aware OpenSky
> client, filters them to Turkish land plus territorial sea with a polygon test, drops military and
> government aircraft before anything is stored, and writes the rest to PostgreSQL/PostGIS with hourly
> roll-ups and request-level metrics. Non-commercial, see *Ethical and legal limits* below.

## Durum

| Aşama | İçerik | Durum |
|---|---|---|
| 1 | Veri toplama: OpenSky istemcisi, Türkiye filtresi, askeri/devlet filtresi, PostGIS, CI | **Geliştiriliyor** |
| 2 | İz takibi ve Kalman filtresi | Planlandı |
| 3 | MapLibre ile canlı harita (WebSocket) | Planlandı |
| 4 | Anomali tespiti (7500/7600/7700, irtifa kaybı, bekleme) | Planlandı |
| 5 | Tekrar oynatma, istatistik, Docker Compose ile yayın, Prometheus/Grafana | Planlandı |

## Mimari (Aşama 1)

```
OpenSky /states/all ──► OpenSkyClient ──► parse_states ──► filter_states ──► PgStore ──► PostgreSQL + PostGIS
  (OAuth2, kutu sorgusu)  (kota, yeniden     (JSON)          1. askeri/devlet at   (positions,
                           deneme, 429)                       2. konumsuz at         positions_hourly,
                                                              3. çokgen dışını at    fetch_log, daily_metrics)
```

| Bileşen | Dosya | Görev |
|---|---|---|
| `OpenSkyClient` | `src/opensky_client.cpp` | OAuth2 istemci kimlik bilgileri, belirteç yenileme, `X-Rate-Limit-*` başlıkları, 5xx/ağ hatasında üstel geri çekilme, 429'da düzgün bekleme |
| `CurlTransport` | `src/curl_transport.cpp` | libcurl ile HTTP; testlerde sahte taşıyıcıyla değiştirilir |
| `MultiPolygon` | `src/geo.cpp` | Türkiye kara sınırı + karasuları çokgeni, delikli çokgende nokta testi |
| `MilitaryFilter` | `src/military_filter.cpp` | ICAO adres blokları, çağrı adı kalıpları, işletici bilgisi ile askeri/devlet uçağı tanıma |
| `PgStore` | `src/pg_store.cpp` | libpqxx ile kayıt, saatlik özetleme ve eski verinin silinmesi |
| `sema_collector` | `apps/collector.cpp` | Kotaya göre kendini ayarlayan toplama döngüsü |

### Sorgu alanı ve kota

Sorgu kutusu çokgenin sınırlarından hesaplanır: yaklaşık kuzey 35,70–42,30, doğu 25,40–44,85
(≈128 derece²). Belgedeki kaba kutu (36–42, 26–45) Karadeniz kıyısını, Hatay'ın güneyini ve
Gökçeada'nın batısını kesiyordu. OpenSky bu büyüklükteki bir kutu için istek başına **3 kredi** alır;
standart hesabın günlük kotası 4000 kredi olduğundan en sık güncelleme yaklaşık **65 saniyede bir**
olur. Toplayıcı bu aralığı kalan krediye ve UTC gece yarısına kalan süreye göre kendisi ayarlar.

### Türkiye sınırı

`data/turkey_boundary.geojson`, `tools/make_boundary.py` ile Natural Earth 1:10m ülke sınırlarından
üretilir: Türkiye kara alanı + 12 deniz mili tampon, eksi komşu ülkelerin kara alanları. Ege'de
karasuyu 6 deniz mili olduğu için çokgen orada denizi biraz fazla kapsar; Yunan adalarının kara
alanı çıkarıldığı için adaların üzerindeki uçaklar dışarıda kalır. Bu Aşama 1 için yeterli bir
yaklaşıklıktır.

## Kurulum

Ubuntu 24.04:

```bash
sudo apt-get install -y cmake ninja-build g++ libcurl4-openssl-dev nlohmann-json3-dev libpqxx-dev libgtest-dev
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -L unit
```

vcpkg ile: `VCPKG_ROOT` ayarlıyken `cmake --preset vcpkg && cmake --build --preset vcpkg`.

Sanitizer'lı derlemeler: `asan` (AddressSanitizer + UBSan) ve `tsan` (ThreadSanitizer) önayarları.

### Veritabanı ve toplayıcı

```bash
cp .env.example .env              # OpenSky API istemci bilgilerini buraya yazın
docker compose up -d db           # PostGIS
set -a; . ./.env; set +a
./build/debug/sema_collector --once --dry-run   # veritabanına yazmadan tek istek
./build/debug/sema_collector                    # sürekli toplama
```

Entegrasyon testleri `SEMA_TEST_DATABASE_URL` tanımlıysa çalışır:

```bash
SEMA_TEST_DATABASE_URL=postgresql://sema:sema@localhost:5432/sema_test ctest --preset debug -L integration
```

Askeri/devlet filtresinin işletici kuralı için OpenSky uçak veritabanı CSV'si
([opensky-network.org/datasets/metadata](https://opensky-network.org/datasets/metadata/)) indirilip
`SEMA_AIRCRAFT_DB` ile gösterilmelidir. Bu dosya büyük olduğu için depoya eklenmez; verilmezse
yalnızca adres bloğu ve çağrı adı kuralları çalışır.

OpenSky API istemci bilgileri OpenSky hesabınızın sayfasından alınır. Bilgiler hiçbir zaman depoya
eklenmez; `.env` dosyası `.gitignore` içindedir.

## Ölçümler

Toplayıcı her isteği `fetch_log` tablosuna yazar; `daily_metrics` görünümü günlük kredi kullanımını,
istek başına uçak sayısını ve gerçek güncelleme aralığını verir:

```sql
SELECT * FROM daily_metrics;
```

| Ölçüt | Değer |
|---|---|
| Günlük kredi kullanımı | henüz ölçülmedi |
| İstek başına ortalama uçak (filtre sonrası) | henüz ölçülmedi |
| Gerçek güncelleme aralığı | henüz ölçülmedi |
| İstek başına atılan askeri/devlet uçağı | henüz ölçülmedi |

Sayılar sistem gerçek veriyle çalışmaya başladıktan sonra buraya eklenecek.

## Etik ve hukuki sınırlar

- **Ticari olmayan kullanım.** OpenSky verisi yalnızca ticari olmayan amaçla kullanılır. Sitede reklam
  ve ücretli özellik yoktur.
- **Tek hesap, kotaya uyum.** Tek bir OpenSky hesabı kullanılır. İstemci kota başlıklarını okur, kota
  bitince OpenSky'ın bildirdiği süre kadar bekler ve kotayı aşacak sıklıkta istek göndermez.
- **Askeri ve devlet uçakları atılır.** Bu uçaklar tanındıkları anda, hiçbir yere kaydedilmeden ve
  gösterilmeden atılır. Filtre, işlem hattında ilk adımdır ve veritabanında bu uçaklara ait satır
  bulunmaz. Tanıma üç kaynağa dayanır: OpenSky uçak veritabanındaki işletici/sahip bilgisi, bilinen
  askeri ICAO adres blokları ve çağrı adı kalıpları (`data/military_rules.json`). Filtre tutucudur:
  tek bir kuralın eşleşmesi yeterlidir. Filtrenin kendi testleri `tests/test_military_filter.cpp`
  içindedir.
- **Askeri amaçlı özellik yoktur.** Üs çevresi izleme, askeri bölge vurgulama veya askeri hareketleri
  çıkarmaya yönelik herhangi bir özellik eklenmez.
- **Kaynak gösterimi.** Veri OpenSky Network'ten gelir ve OpenSky'ın istediği makale aşağıda
  gösterilir.

## Kaynaklar

Bu proje [OpenSky Network](https://opensky-network.org) verisini kullanır:

> Matthias Schäfer, Martin Strohmeier, Vincent Lenders, Ivan Martinovic and Matthias Wilhelm.
> "Bringing Up OpenSky: A Large-scale ADS-B Sensor Network for Research."
> In *Proceedings of the 13th IEEE/ACM International Symposium on Information Processing in Sensor
> Networks (IPSN)*, pages 83–94, April 2014.

Ülke sınırları: [Natural Earth](https://www.naturalearthdata.com) (kamu malı).

## Lisans

Kaynak kod MIT lisanslıdır (bkz. `LICENSE`). OpenSky verisi OpenSky'ın kendi kullanım koşullarına
tabidir.
