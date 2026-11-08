# Semâ — web haritası

Canlı Türkiye hava sahası haritasının tarayıcı istemcisi (TypeScript, Vite, MapLibre GL).
Sunucu `/ws` üzerinden JSON mesajları (hello, coverage, snapshot, events) gönderir; istemci hiçbir şey göndermez.
`events` mesajı süren ve son biten anomali olaylarını (en çok 50) izleriyle birlikte taşır; "Olaylar" düğmesi listeyi açar, bir olay seçilince uçağın o andaki izi haritada gösterilir.
Uçak konumları iki anlık görüntü arasında sabit hızla ileriye doğru tahmin edilerek akıcı biçimde hareket ettirilir.

## Geliştirme

Önce arka ucu 8080 portunda çalıştırın (canlı ya da kayıttan oynatma):

```sh
sema_collector --serve 8080      # canlı veri
# veya
sema_replay --serve 8080         # kayıttan oynatma
```

Sonra:

```sh
cd web
npm ci
npm run dev
```

Vite geliştirme sunucusu `/ws`, `/api`, `/boundary.geojson` ve `/healthz` isteklerini `http://127.0.0.1:8080` adresine yönlendirir.

## Derleme ve test

```sh
npm run typecheck   # tsc --noEmit
npm test            # vitest run
npm run build       # tsc --noEmit && vite build  →  web/dist
```

`web/dist` klasörü arka ucun statik dosya kökü olarak sunulur.

## Ayarlar

- `VITE_MAP_STYLE`: altlık harita stil URL'si (varsayılan `https://tiles.openfreemap.org/styles/positron`).
  Stil yüklenemezse harita yalnızca katmanlarla (sınır, uçaklar, kapsama) çalışmaya devam eder.

Veri: [OpenSky Network](https://opensky-network.org). Ticari olmayan bir projedir; askerî ve devlet hava araçları filtrelenir ve gösterilmez.
