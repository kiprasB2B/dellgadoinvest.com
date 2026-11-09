# Dellgado.invest – paskelbimas internete

Svetainė – viena statinė brošiūra (`public/index.html`), kurią aptarnauja
mažas serveris gryna C kalba (`src/server.c`). HTTPS sertifikatą suteikia
talpinimo platforma, todėl serveris kalba paprastu HTTP savo konteinerio viduje.

```
public/index.html   – svetainė (brošiūra)
src/server.c        – C17 serveris (POSIX sockets + pthreads)
Makefile            – make, make debug, make run, make test
tests/test.sh       – integraciniai testai su curl
Dockerfile          – daugiapakopis vaizdas (gcc → mažas alpine, ne root)
render.yaml         – Render Blueprint (nemokamas planas)
```

---

## 1. Pirmas paskelbimas Render (nemokamai)

1. Užsiregistruokite <https://render.com> (patogiausia – „Sign in with GitHub“).
2. Viršuje spauskite **New → Blueprint**.
3. Prijunkite GitHub paskyrą (jei prašo) ir pasirinkite repozitoriją
   **kiprasB2B/dellgadoinvest.com**. Šaka – **main**.
4. Render perskaitys `render.yaml` ir parodys vieną paslaugą
   **dellgado-invest** (Web Service, Docker, Free). Spauskite
   **Deploy Blueprint** (arba **Apply**).
5. Palaukite, kol žurnale pasirodys `listening on 0.0.0.0:...`, o būsena taps
   **Live** (pirmas kūrimas užtrunka kelias minutes).

> Svarbu: `render.yaml` turi būti **main** šakoje, todėl pirmiausia sujunkite
> (merge) pull request'ą su šiais failais.

### Kur rasti viešą adresą

Render → **Dashboard** → paslauga **dellgado-invest**. Adresas rodomas
puslapio viršuje, po pavadinimu, pvz.:

**https://dellgado-invest.onrender.com**

Jei pavadinimas `dellgado-invest` jau užimtas, Render pats prideda trumpą
priesagą (pvz. `https://dellgado-invest-a1b2.onrender.com`) – tikrasis adresas
visada matomas toje pačioje vietoje.

Patikrinimas: `https://<adresas>/healthz` turi grąžinti `ok`.

### Ką verta žinoti apie nemokamą planą

- Jei ~15 min. niekas neapsilanko, paslauga „užmiega“. Pirmas lankytojas po to
  laukia apie 30–60 s, kol ji pabunda; vėliau viskas veikia greitai.
- Nemokamų valandų per mėnesį pakanka vienai nuolat veikiančiai paslaugai.
- Lankytojų pažymėjimai saugomi jų naršyklėje (localStorage), serveryje
  nieko nesaugoma, todėl užmigimas ar naujas diegimas duomenų neištrina.

---

## 2. Kaip atnaujinti svetainę

Užtenka pakeitimus įkelti į **main** šaką – Render automatiškai perkurs ir
paskelbs naują versiją (`autoDeploy: true`).

- Pakeiskite `public/index.html` (arba įdėkite naujų failų į `public/`).
- Per GitHub: atidarykite failą → pieštuko ikona → **Commit changes**
  (tiesiai į `main` arba per pull request ir **Merge**).
- Arba kompiuteryje:

```bash
git checkout main
```

```bash
git pull
```

```bash
git add public/index.html
```

```bash
git commit -m "Atnaujinta brošiūra"
```

```bash
git push
```

Eigą matysite Render → **dellgado-invest** → **Events / Logs**. Rankiniu būdu
perdiegti galima mygtuku **Manual Deploy → Deploy latest commit**.

Prieš įkeliant galima patikrinti vietoje (reikia gcc, make ir curl):

```bash
make test
```

```bash
make run
```

Tada atsidarykite <http://localhost:8080>.

---

## 3. Savo domeno (dellgadoinvest.com) prijungimas vėliau

1. Render → **dellgado-invest** → **Settings → Custom Domains → Add Custom Domain**.
2. Įrašykite `dellgadoinvest.com`, paskui dar kartą – `www.dellgadoinvest.com`.
3. Render parodys, kokių DNS įrašų reikia. Pas domeno registratorių (DNS
   valdymo skiltyje) sukurkite:

| Tipas | Vardas (host) | Reikšmė |
|-------|---------------|---------|
| `A` (arba `ALIAS`/`ANAME`, jei registratorius jį siūlo) | `@` | adresas, kurį nurodo Render (paprastai `216.24.57.1`) arba `dellgado-invest.onrender.com` ALIAS atveju |
| `CNAME` | `www` | `dellgado-invest.onrender.com` (arba jūsų tikrasis `.onrender.com` adresas) |

   - Ištrinkite senus `A`/`AAAA` įrašus tiems patiems vardams (pvz. registratoriaus
     „parkavimo“ puslapio), kitaip jie konfliktuos.
   - Visada vadovaukitės tuo, ką rodo Render – reikšmės ten yra galutinės.
4. Grįžkite į Render ir spauskite **Verify**. DNS pasikeitimai gali užtrukti
   nuo kelių minučių iki 24–48 val.
5. Patvirtinus domeną, Render automatiškai išduoda nemokamą HTTPS sertifikatą.
   Senasis `.onrender.com` adresas taip pat toliau veiks.

---

## 4. Alternatyva: Koyeb (nemokamas planas, tas pats Dockerfile)

1. Užsiregistruokite <https://www.koyeb.com> ir prisijunkite su GitHub.
2. **Create Service → Web Service → GitHub**, pasirinkite repozitoriją
   **kiprasB2B/dellgadoinvest.com**, šaka **main**.
3. **Builder**: pasirinkite **Dockerfile** (kelias `Dockerfile`, kontekstas – šaknis).
4. **Instance**: **Free**. Regionas – artimiausias (pvz. Frankfurtas).
5. **Ports**: `8080`, protokolas HTTP. **Environment variables**: `PORT=8080`
   (kad sutaptų su atvertu prievadu).
6. **Health checks**: HTTP, kelias `/healthz`, prievadas `8080`.
7. Pavadinkite paslaugą, pvz. `dellgado-invest`, ir spauskite **Deploy**.
8. Viešas adresas bus panašus į `https://dellgado-invest-<jūsų-org>.koyeb.app`
   (matomas paslaugos puslapyje). Kiekvienas `push` į `main` perdiegia
   automatiškai. Savo domeną galima prijungti **Domains** skiltyje – Koyeb
   parodys reikiamą `CNAME` įrašą.

Koyeb nemokamame plane leidžiama viena nemokama paslauga; neaktyvi paslauga
taip pat gali užmigti ir pabusti per pirmą užklausą.

---

## 5. Techninė informacija

- Serveris klausosi `0.0.0.0:$PORT` (numatyta 8080; Render ir Koyeb nustato patys).
- Kiti aplinkos kintamieji (nebūtini): `DOCROOT` (numatyta `public`),
  `REQUEST_TIMEOUT_SEC` (numatyta 10).
- `GET`/`HEAD` – statiniai failai iš `public/`, `/` → `index.html`,
  `/healthz` → `200 ok`, nežinomi keliai → 404 puslapis, kiti metodai → 405.
- Apsauga nuo kelio apėjimo (`../`, `%2e%2e`, absoliutūs keliai, null baitai,
  simbolinės nuorodos už `public/` ribų) su `realpath()` patikra; paslėpti
  failai (`.env`, `.git`) neaptarnaujami.
- Užklausos antraštė ribojama 8 KB (431), skaitymo laikas – 10 s (408).
- Saugumo antraštės: `X-Content-Type-Options`, `Referrer-Policy`,
  `Strict-Transport-Security`, `Content-Security-Policy`.
- SIGTERM → nebepriimamos naujos jungtys, baigiamos esamos (iki 10 s), išeinama
  su kodu 0. Viena žurnalo eilutė per užklausą į stdout.
- Docker vaizdas: kompiliuojama `gcc:14`, galutinis vaizdas `alpine:3.20`
  (~14 MB), vykdoma kaip vartotojas `app` (uid 10001).

```bash
docker build -t dellgado-invest .
```

```bash
docker run --rm -p 8080:8080 dellgado-invest
```
