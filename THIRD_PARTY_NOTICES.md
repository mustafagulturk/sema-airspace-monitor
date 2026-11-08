# Üçüncü taraf yazılım ve veri bildirimleri

Semâ'nın kendi kaynak kodu MIT lisanslıdır (`LICENSE`). Aşağıdakiler başkalarına aittir ve kendi
lisanslarıyla kullanılır. Lisans metinleri her projenin kendi deposunda/paketinde bulunur; Docker
imajında sistem kütüphanelerinin metinleri `/usr/share/doc/<paket>/copyright` altındadır.

## Veri

| Kaynak | Kullanım | Lisans / koşul |
|---|---|---|
| [The OpenSky Network](https://opensky-network.org) | Canlı uçak konumları (REST API), isteğe bağlı uçak veritabanı | [OpenSky General Terms of Use & Data License Agreement](https://opensky-network.org/about/terms-of-use). Kâr amacı gütmeyen araştırma/eğitim; canlı/operasyonel kullanım için yazılı lisans gerekir. Veri bu depoyla **dağıtılmaz**. Atıf: Schäfer ve ark., IPSN 2014. |
| [Natural Earth](https://www.naturalearthdata.com) | `data/turkey_boundary.geojson` (türetilmiş) | Kamu malı |
| [OurAirports](https://ourairports.com/data/) | `data/airports_tr.json` (türetilmiş) | Kamu malı |
| [OpenStreetMap](https://www.openstreetmap.org/copyright) katkıcıları, [OpenMapTiles](https://openmaptiles.org), [OpenFreeMap](https://openfreemap.org) | Harita altlığı (tarayıcı doğrudan OpenFreeMap'ten yükler, depoda yoktur) | OSM verisi ODbL 1.0; OpenMapTiles şeması/stili BSD-3 + CC-BY 4.0; atıf harita köşesinde gösterilir |

## C++ çekirdek (derlenen ikili dosyalara bağlanır)

| Kütüphane | Lisans |
|---|---|
| [libcurl](https://curl.se) | curl lisansı (MIT/X türevi) |
| [OpenSSL](https://www.openssl.org) (libcurl üzerinden) | Apache 2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [Boost](https://www.boost.org) (Beast, Asio) | Boost Software License 1.0 |
| [libpqxx](https://github.com/jtv/libpqxx) | BSD-3-Clause |
| [libpq](https://www.postgresql.org) | PostgreSQL License |
| [GoogleTest](https://github.com/google/googletest) (yalnızca testler) | BSD-3-Clause |

## Web arayüzü (tarayıcıya giden paket)

| Kütüphane | Lisans |
|---|---|
| [MapLibre GL JS](https://maplibre.org) ve bağımlılıkları | BSD-3-Clause (MapLibre), bağımlılıklar MIT/ISC/BSD |

`npm run build` paketlenen her npm kütüphanesinin lisans metnini `web/dist/third-party-licenses.txt`
dosyasına yazar; site bu dosyayı `/third-party-licenses.txt` adresinde sunar (gizlilik sayfasından
bağlantılı).

## Yayın ortamı (ayrı Docker imajları, değiştirilmeden kullanılır)

| Yazılım | Lisans |
|---|---|
| [PostgreSQL](https://www.postgresql.org) + [PostGIS](https://postgis.net) (`postgis/postgis`) | PostgreSQL License, GPL-2.0 (PostGIS) |
| [Caddy](https://caddyserver.com) | Apache 2.0 |
| [Prometheus](https://prometheus.io) | Apache 2.0 |
| [Grafana OSS](https://grafana.com/oss/grafana/) | AGPL-3.0 (değiştirilmeden, ayrı bir servis olarak çalışır; Grafana'nın kaynak kodunda değişiklik yapılırsa AGPL değişikliklerin yayımlanmasını ister) |
| [Ubuntu 24.04](https://ubuntu.com) temel imajı | Paket başına (bkz. `/usr/share/doc/*/copyright`) |
