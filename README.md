# Semâ — Canlı Türkiye Hava Sahası İzleme

[![CI](https://github.com/mustafagulturk/sema-airspace-monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/mustafagulturk/sema-airspace-monitor/actions/workflows/ci.yml)

Semâ, Türkiye üzerindeki gerçek **sivil** uçak trafiğini [OpenSky Network](https://opensky-network.org)'ün
ücretsiz verisiyle canlı izleyen bir C++20 sistemidir. Simülasyon yoktur, her şey gerçek veriyle çalışır.

**Canlı site:** henüz yayında değil; VPS'e kurulunca adres buraya eklenecek (kurulum: [Yayın](#yayın-vps-alan-adı-https)).

> **English summary.** Semâ is a C++20 system that tracks live civil air traffic over Turkey using
> OpenSky Network data. Phase 1 collects state vectors with an OAuth2, quota-aware OpenSky client,
> filters them to Turkish land plus territorial sea with a polygon test, drops military and government
> aircraft before anything is stored, and writes the rest to PostgreSQL/PostGIS with hourly roll-ups and
> request-level metrics. Phase 2 adds per-aircraft tracking: track create/confirm/coast/terminate,
> a constant-velocity Kalman filter and a coordinated-turn EKF scored against dead reckoning on every
> new fix, signal-loss bridging, data-quality rejection (impossible speed, teleports, corrupt
> altitude) and metrics (prediction error in metres, tracked aircraft, track break rate, processing
> time per update), plus record/replay of filtered snapshots. Phase 3 adds the live map: a Boost.Beast
HTTP/WebSocket server inside the C++ collector pushes tracker estimates, short trails and a rolling
24-hour coverage grid to a TypeScript + MapLibre GL front end, which moves each aircraft between
OpenSky updates with the Kalman filter's own constant-velocity prediction. Phase 4 adds anomaly
detection on the tracker's filtered fixes: emergency squawks (7500/7600/7700, confirmed on two
reports), rapid descent, holding/circling (staying inside a small circle at speed, robust to the
~65 s sampling), turn-backs at cruise and long signal loss in well-covered cells. Events are stored
in PostGIS, pushed to the map's event list with the aircraft's track, and counted per 1,000
airborne aircraft-hours so thresholds can be tuned on real recordings. Phase 5 adds history and
deployment: every track becomes a flight record with departure/arrival airport (civil airports only,
air bases excluded) and the corridor cells it crossed; a JSON API on a worker thread serves range
replays from PostGIS (interpolated in the browser at x1–x120) and traffic statistics (daily flights,
busiest hours, airport movements, top routes, corridor heat map); `/metrics` exposes quota, latency,
errors, memory and an unbroken-collection streak to Prometheus/Grafana; a multi-stage Docker image
and one Compose file run PostGIS, the collector, Caddy (automatic HTTPS), Prometheus and Grafana on
a single VPS. Non-commercial, see *Ethical and legal
> limits* below.

## Durum

| Aşama | İçerik | Durum |
|---|---|---|
| 1 | Veri toplama: OpenSky istemcisi, Türkiye filtresi, askeri/devlet filtresi, PostGIS, CI | Tamamlandı |
| 2 | İz takibi: Kalman filtresi, sinyal kaybı, veri kalitesi, ölçümler | Tamamlandı (gerçek veriyle ölçüm bekliyor) |
| 3 | MapLibre ile canlı harita (WebSocket), detay paneli, kapsama haritası | Tamamlandı |
| 4 | Anomali tespiti: acil durum kodları, ani irtifa kaybı, bekleme, rota sapması, uzun sinyal kaybı; olay listesi | Tamamlandı (eşikler gerçek veriyle ayarlanmayı bekliyor) |
| 5 | Geçmişi tekrar oynatma, istatistikler, Docker Compose ile yayın, HTTPS, Prometheus/Grafana | Tamamlandı (VPS'e kurulum ve gerçek veriyle sayılar bekliyor) |

## Sistem mimarisi

```mermaid
flowchart LR
  OS[OpenSky /states/all] -->|OAuth2, kota| C
  subgraph C[sema_collector · C++20]
    direction TB
    F[Türkiye çokgeni +<br/>askeri/devlet filtresi] --> T[Tracker<br/>Kalman KF / EKF]
    T --> A[AnomalyDetector]
    T --> FL[FlightLog<br/>kalkış/iniş, koridor]
    T --> L[LiveState]
    H[HistoryApi<br/>iş parçacığı] 
    M[PromRegistry]
  end
  F -->|positions, fetch_log| DB[(PostgreSQL<br/>+ PostGIS)]
  FL -->|flights| DB
  A -->|anomaly_events| DB
  DB --> H
  L -->|WebSocket /ws| CAD
  H -->|/api/replay, /api/stats| CAD
  CAD[Caddy · HTTPS] --> W[Tarayıcı<br/>TypeScript + MapLibre]
  M -->|/metrics| P[Prometheus] --> G[Grafana]
  CAD -->|/grafana| G
```

Askeri ve devlet uçakları ilk kutuda atılır; sağdaki hiçbir bileşen onları görmez.

## Veri toplama (Aşama 1)

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

## Canlı harita (Aşama 3)

```
Tracker ──► LiveState ──────────────────────► LiveServer (Boost.Beast) ──WebSocket /ws──► tarayıcı (TypeScript + MapLibre GL)
            iz tahmini, kısa iz kuyruğu,        her konunun son mesajı,                    uçak simgeleri, iz kuyrukları,
            kalkış ülkesi, kapsama ızgarası     statik dosyalar, /healthz                  detay paneli, kapsama katmanı
```

| Bileşen | Dosya | Görev |
|---|---|---|
| `LiveState` | `src/live.cpp` | İzleyicinin tahminlerinden harita mesajını kurar: konum, hız, yön, irtifa, belirsizlik, son 20 filtrelenmiş konumdan iz kuyruğu |
| `CoverageGrid` | `src/live.cpp` | Türkiye çokgeniyle kesişen 0,5°'lik hücrelerde son 24 saatin konum sayısı (saatlik halka, sabit bellek) |
| `LiveServer` | `src/live_server.cpp` | Tek G/Ç iş parçacıklı HTTP + WebSocket sunucusu; yeni istemciye her konunun son mesajı, sonra her yeni mesaj; geride kalan istemciyi düşürür |
| `resolve_static_path` | `src/static_files.cpp` | Web dosyalarını kök dizin dışına çıkmadan (`..`, sembolik bağ) sunar |
| Web arayüzü | `web/` | TypeScript, Vite, MapLibre GL; ayrıntı için `web/README.md` |

**Akıcı hareket.** OpenSky yaklaşık 65 saniyede bir güncellenir. Sunucu her güncellemede her iz için
sabit hız Kalman filtresinin o anki durumunu (konum, hız, yön, dikey hız) ve kendi saatini gönderir.
Tarayıcı iki güncelleme arasında uçağı bu durumdan ileri taşır. Sabit hız modelinin tahmini doğrusal
olduğu için tarayıcının hesapladığı konum, sunucudaki filtrenin aynı ana tahminiyle aynıdır; yani
haritadaki hareket görsel bir hile değil, Aşama 2'de hatası ölçülen tahminin kendisidir. Yeni konum
geldiğinde uçak düzeltilmiş tahmine geçer. Sinyali kesilen iz (coasting) en çok 600 saniye tahminle
sürer ve soluk gösterilir.

**Mesajlar** (`kLiveProtocolVersion = 1`, JSON): `hello` (kaynak: canlı/kayıt, OpenSky atfı),
`snapshot` (tüm izler, saat ve hız katsayısı), `coverage` (hücre başına rapor sayısı) ve `events`
(Aşama 4: süren ve son biten anomali olayları, izleriyle). Tam liste
`include/sema/live.hpp` içindedir. Anlık görüntü 300 uçakta sıkıştırmasız ~150 KB'tır; WebSocket
`permessage-deflate` ile sıkıştırılır.

**Detay paneli.** Çağrı adı, ICAO adresi, tescil ülkesi, irtifa, hız, yön, dikey hız, iz durumu,
son görülme ve konum belirsizliği. Kapsam belgesinde "kalkış ülkesi" yazıyordu; OpenSky'ın
`origin_country` alanı ise kalkış yerini değil, ICAO adres bloğuna göre uçağın tescil ülkesini verir.
Durum vektörlerinde kalkış havalimanı yoktur, bu yüzden alan dürüstçe "tescil ülkesi" diye adlandırıldı.

**Kapsama haritası.** Son 24 saatte hiç konum gelmeyen hücre *boşluk* olarak gösterilir. Veriden tek
başına "alıcı yok" ile "trafik yok" ayrılamaz; arayüz bunu açıkça yazar. Ölçüm, Türkiye'nin doğusundaki
bilinen kapsama boşluğunu gerçek veriyle göstermek içindir.

**Güvenlik ve sınırlar.** Sunucu yalnızca GET/HEAD kabul eder, istek başlığı ve gövdesi 8 KB ile,
istemciden gelen WebSocket mesajı 4 KB ile, istemci sayısı 500 ile sınırlıdır. Yavaş istemci, kuyruğu
8 mesajı geçince düşürülür; bellek istemci başına sınırlıdır. Varsayılan dinleme adresi `127.0.0.1`'dir;
HTTPS ve dış erişim Aşama 5'te ters vekil (reverse proxy) ile gelecek.

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

## Anomali tespiti (Aşama 4)

```
Tracker ──► AnomalyDetector ──┬──► anomaly_events (PostGIS) + olay dosyası (JSON Lines)
            5 kural, iz başına ├──► LiveServer "events" konusu ──► haritada olay listesi ve olay anındaki iz
            30 dk konum geçmişi └──► log ("anomaly" satırları, saatlik "anomaly_metrics")
```

Dedektör izleyiciden hemen sonra, **aynı filtrelenmiş** anlık görüntülerle çalışır. Kurallar ham
raporları değil, izleyicinin kalite kontrolünden geçmiş, Kalman ile düzeltilmiş konum ve irtifalarını
kullanır; böylece reddedilen bir "ışınlanma" ya da 30 km'lik bozuk bir irtifa hiçbir kuralı
tetikleyemez. Askeri/devlet uçakları bu noktaya hiç ulaşmaz, yani onlar için olay oluşamaz
(`MilitaryAircraftNeverReachTheDetector` testi).

| Kural | Ne zaman olay açılır | Neden bu eşik |
|---|---|---|
| **Acil durum kodu** | 7500 (yasa dışı müdahale), 7600 (telsiz arızası) veya 7700 (genel acil durum) **art arda 2 raporda**, uçak havadayken | Tek bozuk Mode S yanıtı "kaçırma" alarmı vermesin. Kod 2 rapor boyunca başka olunca olay kapanır. |
| **Ani irtifa kaybı** | Her adımı aşağı giden en az 3 konumda, en çok 4 dk içinde ≥ 1.500 m kayıp ve ortalama ≥ 22 m/s (~4.300 ft/dk), başlangıç ≥ 4.000 m | Normal alçalma 1.500–3.000 ft/dk'dır (8–15 m/s). Acil alçalma 4.000–6.000+ ft/dk'dır. Düşük irtifadaki yaklaşma dışarıda kalır. |
| **Bekleme / daire çizme** | En az 8 dk ve 6 konum boyunca tüm konumlar merkezlerine ≤ 15 km, ortalama yer hızı ≥ 60 m/s | Bu hızla 8 dakikada 30+ km yol alınır; 15 km'lik dairede kalmak tur atmak demektir. Yön değişimini saymak ~65 s örneklemede yanıltır (bir dönüş iki rapor arasında başlayıp biter); geometrik ölçüt buna dayanıklıdır. 60 m/s altı (eğitim uçakları, helikopterler) dışarıda kalır. |
| **Rota sapması** | Seyir irtifasında (≥ 6.000 m) en az 4 konumluk düz bir rotadan (±12°) sonra ≥ 120° farklı yeni bir rota iki bacak boyunca tutulursa | Hava yolu dönüşleri çoğunlukla 90°'nin altındadır; seyirde geri dönmek bir dönüş ya da başka meydana yönelme görüntüsüdür. Kalkış/yaklaşmadaki rutin ters dönüşler irtifa sınırının altında kalır, bekleme sürerken bu kural çalışmaz. |
| **Uzun sinyal kaybı** | Onaylı ve ≥ 3.000 m'deki iz 5 dk konum alamaz, tahmini konumu hâlâ Türkiye içindeyse ve son konumunun 0,5°'lik hücresi son 24 saatte ≥ 200 rapor almışsa | Doğudaki kapsama boşlukları (bkz. kapsama haritası) yüzünden her kayıp olay olmasın. Bu kural ilk saatlerde, kapsama sayıları birikene kadar sessizdir. Sinyal dönünce ya da iz bitince olay kapanır. |

Aynı uçakta aynı türden ikinci olay, öncekinin bitiminden 15 dk geçmeden açılmaz (kısa süre kuralın
dışına çıkıp geri giren bir alçalma tek olaydır). Her olay, başlangıcından 15 dk öncesinden bitişine
kadar uçağın izini (en çok 120 nokta) taşır; harita olayı seçince bu izi kesikli çizgiyle gösterir.
Tüm eşikler `AnomalyConfig` içindedir (`include/sema/anomaly.hpp`).

**Yanlış alarm oranı ve eşik ayarı.** Olay sayısı tek başına anlamsızdır; dedektör, havadaki onaylı
izleri zamanla çarpıp **uçak-saat** biriktirir ve her türü *1.000 uçak-saat başına olay* olarak
verir. Eşikler gerçek bir kayıtla şöyle ayarlanır:

1. Toplayıcıyla en az birkaç günlük kayıt alınır (`--record kayit.jsonl`).
2. `sema_replay kayit.jsonl --events` her olayı zaman, çağrı adı ve özetle listeler.
3. Her olay elle kontrol edilir (izine ve, varsa, açık kaynak haberlere bakılarak) ve gerçek/yanlış
   diye işaretlenir.
4. Yanlış alarm veren kuralın eşiği `--anomaly AD=DEĞER` ile değiştirilip (ör.
   `--anomaly descent_min_rate_ms=25`) aynı kayıt yeniden oynatılır; gerçek olayları kaybetmeden yanlış
   alarmları düşüren değer `AnomalyConfig`'e yazılır. `--anomaly list` tüm eşik adlarını verir.

Gerçek veriyle yapılan ayar ve sonuçları "Ölçümler" bölümüne eklenecek; şu anki değerler yukarıdaki
gerekçelere dayanan başlangıç değerleridir.

## Geçmiş, istatistik ve yayın (Aşama 5)

```
Tracker ──► FlightLog ──► flights (PostGIS) ──┐
                                              ├──► HistoryApi (iş parçacığı) ──► /api/stats   ──► İstatistik ekranı
positions (ham, 7 gün) ───────────────────────┤                               ──► /api/replay  ──► Geçmiş ekranı
fetch_log ────────────────────────────────────┘                               ──► /api/status  ──► kesintisiz süre
PromRegistry ──► /metrics ──► Prometheus ──► Grafana
```

| Bileşen | Dosya | Görev |
|---|---|---|
| `FlightLog` | `src/flights.cpp` | İzleyicinin her izini bir uçuş kaydına çevirir: ilk/son konum, irtifa aralığı, kalkış ve iniş havalimanı, geçtiği 0,25°'lik koridor hücreleri |
| `AirportIndex` | `src/flights.cpp`, `data/airports_tr.json` | Tarifeli seferi olan 50 sivil havalimanı (`tools/make_airports.py`, OurAirports); hava üsleri listede yoktur |
| `compute_traffic_stats` | `src/traffic_stats.cpp` | Günlük uçuş, saatlere göre yoğunluk (TSİ), havalimanı hareketleri, en yoğun rotalar, koridor yoğunluğu; tek bir saf fonksiyon |
| `MemoryHistory`, `PgHistory` | `src/history.cpp`, `src/pg_history.cpp` | Geçmiş kaynağı: veritabanısız denemede ve `sema_replay`'de bellek, yayında PostgreSQL |
| `HistoryApi` | `src/history.cpp` | `/api/status`, `/api/replay`, `/api/stats`; aralık doğrulama, dakikalık önbellek |
| `LiveServer` (ek) | `src/live_server.cpp` | API isteklerini ayrı bir iş parçacığında yanıtlar; yavaş bir sorgu WebSocket istemcilerini bekletmez |
| `PromRegistry` | `src/prom_metrics.cpp` | Prometheus metin biçiminde `/metrics` |
| Web | `web/src/replay.ts`, `web/src/stats.ts` | Geçmiş oynatıcı ve istatistik ekranı |

**Uçuş, kalkış ve iniş.** Bir uçuş, bir izin ilk ve son kabul edilmiş konumu arasıdır; yalnızca havada
en az iki konumu olanlar sayılır. İlk konum bir havalimanına 12 km'den yakın, havalimanı
yüksekliğinin en çok 1.500 m üstünde (ya da yerde) ve uçak alçalmıyorsa (dikey hız > −2,5 m/s) o
havalimanından **kalkış** sayılır; son konum için aynısı tırmanmıyorsa **iniş** sayılır. Böylece
yaklaşmadayken yakalanan bir uçak "kalkış", kalkıştan hemen sonra kaybolan bir uçak "iniş" olmaz
(`tests/test_flights.cpp`). ~65 saniyelik güncelleme aralığında uçak kalkıştan sonraki ilk raporda
genellikle 1.000 m'nin altındadır, bu yüzden eşik bilerek geniştir. Sinyal 10 dakikadan uzun
kesilirse iz biter ve aynı uçak iki uçuş sayılabilir; bu sayı Aşama 2'deki iz kopma oranıyla aynıdır
ve README'de gerçek veriyle verilecektir.

**İstatistikler.** Günler ve saatler Türkiye saatidir (UTC+3). *Saatlere göre yoğunluk*, o saat
diliminde havada olan uçuşların gün başına ortalamasıdır. *Koridorlar*, 0,25°'lik her hücreden geçen
farklı uçuş sayısıdır; iki rapor arasındaki doğru parçası 5 km'de bir örneklenir, hızlı uçak hücre
atlamaz. Kapsama boşluğu olan bölgelerde trafik eksik görünür. Sayılar `flights` tablosundan
hesaplanır ve süren uçuşlar da eklenir. Ham konumlar `SEMA_RETENTION_HOURS` kadar (yayında 168 saat)
tutulur ve özetlenip silinir; uçuş kayıtları küçüktür (uçuş başına ~300 bayt) ve kalıcıdır, bu yüzden
istatistikler aylarca geriye gider. `psql` içinde: `SELECT * FROM daily_traffic;`.

**Geçmişi tekrar oynatma.** Harita üstteki **Geçmiş** düğmesiyle istenen başlangıç ve süreyi (30 dk –
4 sa) `/api/replay`'den alır ve x1–x120 hızla oynatır; zaman çubuğuyla ileri geri gidilir. Canlı
görünümden farklı olarak her aralığın iki ucu da bilindiği için uçak, son rapordan tahminle değil iki
gerçek rapor arasında ara değerlemeyle konumlanır. 10 dakikadan uzun sinyal boşluğunda uçak
gösterilmez. Yanıt kompakt dizilerdir (`[t, enlem, boylam, irtifa, hız, yön, yerde]`); 300 uçaklı bir
saat sıkıştırılmadan ~1 MB, Caddy'nin gzip/zstd sıkıştırmasıyla bunun küçük bir kısmıdır.

**Kesintisiz çalışma.** `fetch_log`'daki başarılı istekler arasında 15 dakikadan uzun ara olmayan son
dönem "kesintisiz veri toplama" süresidir; arayüzün İstatistik ve Hakkında panellerinde ve Grafana'da
gösterilir. Kısa bir yeniden başlatma (yeni sürüm kurmak gibi) bu süreyi bozmaz, çünkü ölçülen şey
sürecin değil veri akışının sürekliliğidir. Ayrıca sürecin kendi çalışma süresi ayrı bir metriktir.

**Metrikler** (`/metrics`, yalnızca Docker ağı içinden): `sema_opensky_requests_total{status}`,
`sema_opensky_request_seconds_total`, `sema_opensky_credits_remaining`,
`sema_opensky_credits_used_total`, `sema_aircraft_kept`, `sema_tracks{state}`,
`sema_tracker_processing_seconds`, `sema_flights_completed_total`, `sema_quality_rejects_total`,
`sema_dropped_military_total`, `sema_db_errors_total`, `sema_live_clients`,
`sema_last_data_time_seconds`, `sema_process_resident_memory_bytes`,
`sema_collection_streak_seconds`, `sema_start_time_seconds`. Grafana panosu
(`deploy/grafana/dashboards/sema.json`) bunlardan kota, gecikme, hata, uçak sayısı ve **bellek**
grafiklerini çizer; iki haftalık çalışmada RSS grafiğinin düz kalması bellek sızıntısı olmadığının
kanıtı olarak kullanılacaktır (birim testler ayrıca ASan ve TSan ile çalışır).

## Kurulum

Ubuntu 24.04:

```bash
sudo apt-get install -y cmake ninja-build g++ libcurl4-openssl-dev nlohmann-json3-dev libpqxx-dev libgtest-dev libboost-dev
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -L unit
```

Web arayüzü için Node.js 22: `cd web && npm ci && npm run build` (ayrıntı `web/README.md`).

vcpkg ile: `VCPKG_ROOT` ayarlıyken `cmake --preset vcpkg && cmake --build --preset vcpkg`.

Sanitizer'lı derlemeler: `asan` (AddressSanitizer + UBSan) ve `tsan` (ThreadSanitizer) önayarları.

### Geçmiş ve istatistik (yerelde)

OpenSky bilgileri olmadan, sentetik kayıtla:

```bash
./build/release/sema_replay tests/fixtures/replay_synthetic.jsonl --serve 8080 --label "sentetik veri"
# http://127.0.0.1:8080/  →  üstte "Geçmiş" ve "İstatistik"
./build/release/sema_replay kayit.jsonl --stats      # aynı istatistikler JSON olarak
```

`sema_collector --dry-run --serve 8080` geçmişi bellekte tutar (son 24 saat); veritabanıyla çalışınca
geçmiş ve istatistik PostGIS'ten gelir.

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

### Ölçüm raporu ve olay etiketleme

README'deki ölçüm tabloları elle taşınmaz; aynı kayıttan tek komutla üretilir:

```bash
# 1. Olayları etiketlemek için CSV'ye dök (son sütun "etiket" boş gelir)
./build/release/sema_replay kayit.jsonl --events-csv olaylar.csv
# 2. olaylar.csv'yi bir tablo programında aç, her olaya "gerçek" ya da "yanlış" yaz
#    (Excel'in noktalı virgüllü CSV'si de okunur; boş bırakılan satır "etiketsiz" sayılır)
# 3. Raporu üret: kayıt, iz takibi, anomali (tür başına yanlış alarm oranıyla) ve trafik tabloları
./build/release/sema_replay kayit.jsonl --report rapor.md --labels olaylar.csv
```

Etiketler olayın türü, adresi ve başlangıç zamanıyla eşlenir; eşikleri `--anomaly` ile değiştirip
`--events-csv olaylar.csv --labels olaylar.csv` ile yeniden döküldüğünde eski etiketler korunur, yeni
olaylar boş gelir. Rapor README'ye kopyalanmak için yazıldığından içinde uçak adresi ve çağrı adı
yoktur; bunlar yalnızca yerelde kalan CSV'dedir. Kayıt, CSV ve rapor dosyaları `.gitignore`'dadır.

### Canlı harita

```bash
(cd web && npm ci && npm run build)                         # web/dist
./build/release/sema_collector --dry-run --serve 8080       # http://127.0.0.1:8080/
```

OpenSky bilgileri olmadan arayüzü denemek için bir kayıt hızlandırılmış oynatılabilir. Harita bu
durumda üstte "Kayıttan oynatma" bandı gösterir:

```bash
./build/release/sema_replay tests/fixtures/replay_synthetic.jsonl --serve 8080 --speed 10 --label "sentetik veri"
```

### Olaylar

```bash
./build/release/sema_collector --dry-run --serve 8080 --events olaylar.jsonl   # ya da SEMA_EVENTS_FILE
./build/release/sema_replay kayit.jsonl --events                               # olay listesi ve oranlar
./build/release/sema_replay kayit.jsonl --events --anomaly holding_max_radius_m=12000
# Her kural için bir sentetik olay içeren kayıt (90 dk olduğu için kapsama eşiği düşürülür):
./build/release/sema_replay tests/fixtures/anomaly_synthetic.jsonl --events --loss-min-coverage 3
./build/release/sema_replay tests/fixtures/anomaly_synthetic.jsonl --loss-min-coverage 3 --serve 8080 --speed 30 --label "sentetik veri"
```

Veritabanı kullanılıyorsa olaylar `anomaly_events` tablosuna yazılır (açılışta, sürerken ve
kapanışta güncellenir); `daily_events` görünümü günlük ve tür başına sayıları verir:

```sql
SELECT * FROM daily_events;
SELECT start_time, kind, callsign, summary FROM anomaly_events ORDER BY start_time DESC LIMIT 20;
```

Arayüz geliştirirken `cd web && npm run dev` Vite sunucusunu açar ve `/ws` isteklerini 8080'deki
sunucuya yönlendirir.

Askeri/devlet filtresinin işletici kuralı için OpenSky uçak veritabanı CSV'si
([opensky-network.org/datasets/metadata](https://opensky-network.org/datasets/metadata/)) indirilip
`SEMA_AIRCRAFT_DB` ile gösterilmelidir. Bu dosya büyük olduğu için depoya eklenmez; verilmezse
yalnızca adres bloğu ve çağrı adı kuralları çalışır.

OpenSky API istemci bilgileri OpenSky hesabınızın sayfasından alınır. Bilgiler hiçbir zaman depoya
eklenmez; `.env` dosyası `.gitignore` içindedir.

### Yayın (VPS, alan adı, HTTPS)

Tek bir küçük VPS yeterlidir (2 vCPU, 2–4 GB RAM, 40 GB disk). **Önce erişimi deneyin:** OpenSky bazı
büyük bulut sağlayıcılarının IP'lerini engelleyebilir; sunucuda
`curl -sI https://opensky-network.org/api/states/all?lamin=40&lomin=28&lamax=41&lomax=29` 200
dönmelidir.

1. Alan adınız için bir A (ve varsa AAAA) kaydını sunucunun IP'sine yönlendirin.
2. Sunucuda Docker ve Compose eklentisini kurun, 80 ve 443 portlarını açın.
3. Depoyu klonlayın, `.env.example`'ı `.env` olarak kopyalayıp doldurun: `OPENSKY_CLIENT_ID`,
   `OPENSKY_CLIENT_SECRET`, `POSTGRES_PASSWORD`, `GRAFANA_ADMIN_PASSWORD`, `SEMA_DOMAIN`.
4. Başlatın:

```bash
docker compose -f docker-compose.prod.yml up -d --build
docker compose -f docker-compose.prod.yml logs -f collector     # her döngüde bir JSON satırı
```

Caddy sertifikayı kendisi alır ve yeniler. Site `https://SEMA_DOMAIN/`, sistem panosu
`https://SEMA_DOMAIN/grafana/` adresindedir (ziyaretçiler giriş yapmadan yalnızca görüntüleyebilir).
`/metrics` dışarıya kapalıdır. Güncelleme: `git pull && docker compose -f docker-compose.prod.yml up -d --build`.

| Servis | İmaj | Görev |
|---|---|---|
| `collector` | bu depo (`Dockerfile`, çok aşamalı, ~140 MB, root olmayan kullanıcı) | Toplayıcı, harita, API, `/metrics` |
| `db` | `postgis/postgis:16-3.4` | Veritabanı (dışarıya kapalı) |
| `caddy` | `caddy:2.10` | HTTPS, sıkıştırma, güvenlik başlıkları, WebSocket |
| `prometheus` | `prom/prometheus` | 15 s'de bir metrik, 60 gün saklama |
| `grafana` | `grafana/grafana-oss` | Hazır "Semâ — sistem sağlığı" panosu |

CI her değişiklikte imajı derler, sentetik kayıtla duman testi yapar (`/`, `/api/status`,
`/api/stats`) ve Compose dosyasını doğrular.

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

### Anomali tespiti

Gerçek veriyle ölçüm henüz yapılmadı. Aşağıdaki iki satır **sentetik** kayıtlardan gelir ve yalnızca
kuralların doğru olayları bulup başka olay üretmediğini gösterir; gerçek bir olay oranı değildir.

| Kayıt (sentetik) | Beklenen | Bulunan |
|---|---|---|
| `anomaly_synthetic.jsonl` (90 dk, 38 uçak, `tools/make_anomaly_fixture.py`) | 7700, ani alçalma (35 m/s), geri dönüş, bekleme, 9 dk sinyal kaybı; tek raporluk 7500 *yok sayılmalı*; 30 normal uçuş sessiz kalmalı | 5/5 olay, başka olay yok, tek 7500 yok sayıldı |
| `replay_synthetic.jsonl` (Aşama 2'nin kaydı, bilerek bozulmuş raporlarla) | Bekleme turları (SMX601); ışınlanma, 30 km irtifa, 950 m/s hız ve adres çakışması olay *üretmemeli* | Yalnızca bekleme |

Dedektörün anlık görüntü başına işlem süresi bu kayıtlarda medyan ~50–60 µs'dir (Release, anlık görüntü başına 19–28 uçak).

| Ölçüt (gerçek veri) | Değer |
|---|---|
| Tür başına olay / 1.000 uçak-saat | henüz ölçülmedi |
| Elle kontrolde yanlış alarm oranı | henüz ölçülmedi |
| Tespit edilen olaylar (örnekler) | henüz ölçülmedi |

### Trafik istatistikleri

Gerçek veriyle ölçüm henüz yapılmadı. Aşağıdaki satır **sentetik** kayıttan
(`sema_replay tests/fixtures/replay_synthetic.jsonl --stats`) gelir ve yalnızca hesaplamanın uçtan uca
çalıştığını gösterir.

| Ölçüt (sentetik, 90 dk) | Değer |
|---|---|
| Uçuş / farklı uçak | 42 / 40 |
| Kalkışı görülen / inişi görülen | 18 / 26 |
| En çok hareket | Trabzon (1 kalkış, 4 iniş) |

| Ölçüt (gerçek veri) | Değer |
|---|---|
| Günlük uçuş sayısı (Türkiye üzerinde) | henüz ölçülmedi |
| En yoğun saatler (TSİ) | henüz ölçülmedi |
| En yoğun havalimanları ve koridorlar | henüz ölçülmedi |
| Kesintisiz çalışma, bellek (RSS) eğilimi | henüz ölçülmedi |

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
  çıkarmaya yönelik herhangi bir özellik eklenmez. Harita yalnızca filtreden geçmiş sivil uçakları
  alır: `LiveState` izleyiciyle aynı filtrelenmiş anlık görüntülerle beslenir, askeri/devlet uçağı
  sunucuya hiç ulaşmaz. Haritada üs, askeri bölge veya benzeri katman yoktur.
- **Olaylar yalnızca sivil uçaklar içindir ve resmî bilgi değildir.** Anomali dedektörü askeri/devlet
  filtresinden sonra çalışır; askeri bölge, üs çevresi veya askeri hareketle ilgili hiçbir kural yoktur.
  Olaylar açık veriden otomatik çıkarılır, yanlış alarm olabilir; arayüz bunu olay ayrıntısında ve
  "Hakkında" panelinde açıkça yazar.
- **İstatistikler hava üslerini kapsamaz.** Havalimanı listesi OurAirports'tan yalnızca tarifeli
  seferi olan sivil havalimanlarıyla üretilir; adında ya da etiketinde "Air Base"/"Army" geçen her yer
  çıkarılır (`tools/make_airports.py`, `LoadsShippedListWithoutAirBases` testi). Ortak kullanımlı
  meydanlarda da yalnızca sivil hareketler sayılır, çünkü askeri/devlet uçakları istatistikten önce
  atılmıştır. Geçmiş oynatma ve istatistikler aynı filtrelenmiş veriden gelir.
- **Ham veri yeniden dağıtılmaz.** Gerçek OpenSky kayıtları (`kayit*.jsonl`) ve olay etiketleri
  yerelde kalır, depoya konmaz (`.gitignore`). Depodaki kayıtlar `tools/` altındaki betiklerle üretilmiş
  sentetik verilerdir. README'ye yalnızca toplu sayılar girer; `sema_replay --report` çıktısında uçak
  adresi ve çağrı adı bulunmaz, çünkü özel bir uçağın olayı bir kişiyi işaret edebilir.
- **Kaynak gösterimi.** Veri OpenSky Network'ten gelir ve OpenSky'ın istediği makale aşağıda
  gösterilir. Aynı atıf haritanın köşesinde ve "Hakkında" panelinde de yer alır.

## Kaynaklar

Bu proje [OpenSky Network](https://opensky-network.org) verisini kullanır:

> Matthias Schäfer, Martin Strohmeier, Vincent Lenders, Ivan Martinovic and Matthias Wilhelm.
> "Bringing Up OpenSky: A Large-scale ADS-B Sensor Network for Research."
> In *Proceedings of the 13th IEEE/ACM International Symposium on Information Processing in Sensor
> Networks (IPSN)*, pages 83–94, April 2014.

Ülke sınırları: [Natural Earth](https://www.naturalearthdata.com) (kamu malı). Havalimanları:
[OurAirports](https://ourairports.com/data/) (kamu malı). Harita altlığı:
[OpenFreeMap](https://openfreemap.org), © [OpenMapTiles](https://openmaptiles.org),
© [OpenStreetMap katkıcıları](https://www.openstreetmap.org/copyright).

## Lisans

Kaynak kod MIT lisanslıdır (bkz. `LICENSE`). OpenSky verisi OpenSky'ın kendi kullanım koşullarına
tabidir.
