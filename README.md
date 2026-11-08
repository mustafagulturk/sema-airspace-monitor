# Semâ — Canlı Türkiye Hava Sahası İzleme

[![CI](https://github.com/mustafagulturk/sema-airspace-monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/mustafagulturk/sema-airspace-monitor/actions/workflows/ci.yml)

Semâ, Türkiye üzerindeki gerçek **sivil** uçak trafiğini [OpenSky Network](https://opensky-network.org)'ün
ücretsiz verisiyle canlı izleyen bir C++20 sistemidir. Simülasyon yoktur, her şey gerçek veriyle çalışır.

> **English summary.** Semâ is a C++20 system that tracks live civil air traffic over Turkey using
> OpenSky Network data. Phase 1 collects state vectors with an OAuth2, quota-aware OpenSky client,
> filters them to Turkish land plus territorial sea with a polygon test, drops military and government
> aircraft before anything is stored, and writes the rest to PostgreSQL/PostGIS with hourly roll-ups and
> request-level metrics. Phase 2 adds per-aircraft tracking: track create/confirm/coast/terminate,
> a constant-velocity Kalman filter and a coordinated-turn EKF scored against dead reckoning on every
> new fix, signal-loss bridging, data-quality rejection (impossible speed, teleports, corrupt
> altitude) and metrics (prediction error in metres, tracked aircraft, track break rate, processing
> time per update), plus record/replay of filtered snapshots. Non-commercial, see *Ethical and legal
> limits* below.

## Durum

| Aşama | İçerik | Durum |
|---|---|---|
| 1 | Veri toplama: OpenSky istemcisi, Türkiye filtresi, askeri/devlet filtresi, PostGIS, CI | Tamamlandı |
| 2 | İz takibi: Kalman filtresi, sinyal kaybı, veri kalitesi, ölçümler | **Geliştiriliyor** |
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

## İz takibi (Aşama 2)

```
filtrelenmiş anlık görüntü ──► check_measurement ──► Tracker ──────────────────► TrackView (tahmini konum)
 (sivil, Türkiye içi)          imkânsız hız,          iz başına:                   + TrackerMetrics
                               ışınlanma,             sabit hız Kalman (KF)
                               bozuk irtifa           koordineli dönüş EKF
                                                      dikey KF (irtifa)
```

| Bileşen | Dosya | Görev |
|---|---|---|
| `Tracker` | `src/tracker.cpp` | ICAO adresine göre iz açma, onaylama, sinyal kaybında "coasting", yeniden yakalama, sonlandırma |
| `HorizontalFilter` | `src/kalman.cpp` | Yerel doğu/kuzey düzleminde sabit hız Kalman filtresi (konum + OpenSky hız/yön ölçümü) |
| `TurnFilter` | `src/kalman.cpp` | Hız, yön ve dönüş hızı tutan koordineli dönüş genişletilmiş Kalman filtresi |
| `VerticalFilter` | `src/kalman.cpp` | İrtifa ve dikey hız için Kalman filtresi |
| `check_measurement` | `src/quality.cpp` | Veri kalitesi kontrolleri |
| `Distribution`, `TrackerMetrics` | `src/metrics.cpp`, `src/tracker.cpp` | Tahmin hatası, izlenen uçak, kopma oranı, işlem süresi |
| `SnapshotWriter/Reader` | `src/snapshot_io.cpp` | Filtrelenmiş anlık görüntüleri JSON Lines olarak kaydetme ve okuma |
| `sema_replay` | `apps/replay.cpp` | Kayıt üzerinde izleyiciyi çalıştırıp ölçümleri basar |

**İz yaşam döngüsü.** OpenSky her uçağı 24 bitlik ICAO adresiyle verdiği için veri ilişkilendirme
sorunu yoktur; iş, filtreleme ve iz yaşam döngüsündedir. İlk konumla iz *geçici* açılır, ikinci
konumla *onaylanır*. Bir anlık görüntüde yeni konum gelmezse iz *coasting* durumuna geçer ve
tahminle sürer; 600 saniye içinde yeni konum gelirse aynı iz devam eder (yeniden yakalama). Daha uzun
süren boşlukta iz sonlandırılır. Sonlandırma nedeni ayrılır:

| Neden | Koşul |
|---|---|
| `landed` | Son rapor yerdeyse ya da uçak 1200 m'nin altında alçalıyorsa |
| `left_area` | Son konumdan 90 s sonraki tahmini konum Türkiye çokgeninin dışındaysa |
| `signal_lost` | Diğer durumlar: kapsama boşluğu |
| `restarted` | Aynı adresten iki ardışık, kendi içinde tutarlı ama izle çelişen rapor geldiyse (adres çakışması ya da bozuk iz) |

Bir iz `signal_lost` ile bittikten sonraki 30 dakika içinde aynı uçak yeni izle dönerse bu bir
**iz kopması** sayılır. *İz kopma oranı* = kopma sayısı / açılan iz sayısı.

**Veri kalitesi kontrolleri** (`QualityLimits`). Bozuk alan atılır, geri kalanı kullanılır:

| Kontrol | Eşik | Sonuç |
|---|---|---|
| Geçersiz konum | NaN, enlem/boylam aralık dışı | Rapor atılır |
| Işınlanma | Son kabul edilen konumdan `500 m + 400 m/s × Δt`'den uzak | Konum atılır (iki ardışık tutarlı rapor izi yeniden başlatır) |
| İmkânsız hız | Yer hızı > 340 m/s | Hız ve yön atılır, konum kullanılır |
| Bozuk irtifa | −400 m altı, 15 500 m üstü ya da `150 m + 125 m/s × Δt`'den hızlı değişim | İrtifa atılır |
| Bozuk dikey hız | \|dikey hız\| > 125 m/s | Dikey hız atılır |

Eşikler sivil trafik için cömerttir; gerçek bir uçağı reddetmemek, yalnızca bozuk veriyi ayıklamak
amaçlanır. Aynı zaman damgasıyla tekrarlanan rapor (OpenSky yeni konum gelene kadar eskisini
tekrarlar) hata değil *bayat* olarak sayılır.

**Tahmin hatası nasıl ölçülür.** Onaylanmış bir ize yeni konum geldiğinde, konum filtreye
verilmeden *önce* her yöntemin o ana tahmini gerçek konumla karşılaştırılır (metre, büyük daire
uzaklığı). Dört yöntem aynı raporlarda ölçülür: sabit hız KF, koordineli dönüş EKF, ölü hesap (son
ham konumu bildirilen hız ve yönle ilerletmek) ve "son konumda bekle". Sonuçlar ufka göre (0–15 s,
15–45 s, 45–90 s, 90–180 s, 180–600 s) ayrıca verilir. Hangi filtrenin izin konumunu verdiği
`TrackerConfig::model` ile seçilir; varsayılan sabit hız KF'dir (aşağıdaki ölçümlere bakın). Dönüş
EKF'sinin dönüş hızı tahmini her durumda `TrackView::turn_rate_dps` ile verilir (Aşama 4'teki bekleme
tespiti için).

Filtreler her güncellemede yerel teğet düzlemin merkezini izin yeni konumuna taşır; böylece
düzlem yaklaşımı hatası metrenin altında kalır.

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

### Kayıt ve tekrar oynatma

Toplayıcı her döngüde izleyiciyi de çalıştırır ve iz sayaçlarını log satırına yazar; tüm ölçümler
saatte bir `tracker_metrics` olayı olarak loglanır. Ölçümleri gerçek veriyle tekrar üretmek için
filtrelenmiş anlık görüntüler kaydedilip `sema_replay` ile oynatılır:

```bash
./build/release/sema_collector --dry-run --record kayit.jsonl     # ya da SEMA_RECORD_FILE=kayit.jsonl
./build/release/sema_replay kayit.jsonl                            # tablo
./build/release/sema_replay kayit.jsonl --json                     # tüm ölçümler JSON
./build/release/sema_replay kayit.jsonl --accel-psd 10 --turn-psd 1e-6   # filtre ayarı denemeleri
```

Kayıt dosyası yalnızca filtreden geçmiş sivil ve Türkiye içi uçakları içerir; askeri/devlet uçakları
kayda hiç girmez.

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

### İz takibi

Aşağıdaki sayılar **sentetik** kayıttan (`tests/fixtures/replay_synthetic.jsonl`, 80 anlık görüntü,
90 dakika, 40 uçak) gelir ve yalnızca yöntemin çalıştığını gösterir; gerçek OpenSky kaydıyla
yenilenecektir. Kayıt `tools/make_replay_fixture.py` ile üretilir: dönüş hızı sınırlı (3°/s) uçuşlar,
tırmanma ve alçalma, OpenSky gibi tam saniyeye kesilmiş zaman damgaları, ~10 m ADS-B ve ~100 m MLAT
gürültüsü ve bilinen sayıda eklenmiş hata (sinyal kaybı, ışınlanma, bozuk irtifa/hız, adres
çakışması, İstanbul yakınında bekleme turları).

| Ölçüt (sentetik) | Değer |
|---|---|
| Tahmin hatası, sabit hız KF (~67 s ufuk) | medyan 142 m, p90 514 m, p95 835 m |
| Tahmin hatası, koordineli dönüş EKF | medyan 180 m, p90 518 m, p95 858 m |
| Tahmin hatası, ölü hesap (taban çizgisi) | medyan 143 m, p90 490 m, p95 804 m |
| Tahmin hatası, son konumda bekle (taban çizgisi) | medyan 13,8 km |
| İrtifa tahmin hatası | medyan 3 m, p90 43 m |
| İzlenen uçak (anlık görüntü başına) | medyan 28, en çok 31 |
| İz kopma oranı | %2,4 (42 izde 1 kopma; 5 sinyal kaybının 4'ü köprülendi) |
| İşlem süresi (Release) | anlık görüntü başına medyan 142 µs, rapor başına ~6 µs |

Bu sentetik veride çıkan iki ders, gerçek veriyle doğrulanacak:

- **Medyan hatanın tabanı zaman damgasıdır.** OpenSky konum zamanını tam saniye verir; iki raporun
  zaman farkı ±1 s hatalıdır. 230 m/s'de bu tek başına ~95 m (1σ) eder. Bu yüzden düz uçuşta hiçbir
  filtre ~100 m'nin altına inemez.
- **~67 s aralıkla dönüş modeli yardım etmiyor.** Yolcu uçaklarının dönüşleri 10–60 s sürer; iki
  rapor arasında başlayıp biten bir dönüşü EKF bir sonraki aralığa taşıyarak hatayı büyütüyor.
  Sabit dönüşlerde (birim testte 1°/s) EKF sabit hız modelinin yarısından az hata verir, bu yüzden
  bekleme tespiti için tutuluyor. OpenSky hız ve yönü doğrudan verdiği için Kalman'ın ölü hesaba göre
  kazancı da küçük; asıl katkısı gürültülü (MLAT) konumları yumuşatmak, hız raporu olmayan uçakta hızı
  kestirmek, belirsizlik vermek ve haritada raporlar arasını doldurmaktır.

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
