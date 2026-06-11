# Web dashboard

`index.html` — a self-contained (inline CSS/JS, no external assets) dashboard
served by the KlaussCPU HTTP/HTTPS file server. It polls the JSON control API
in [`../webapi.c`](../webapi.c) and shows uptime, switches, live performance
counters (CPI / utilisation / instruction mix / cache, with interpretation), and
lets you drive the LEDs and 7-segment display.

It is **board content**, not compiled into the firmware — deploy it to the SD
card (served as the directory index at `/`):

```sh
# HTTPS is the intended path (trusted cert; the plain :80 server is single-
# threaded and a browser holding it open wedges uploads).
curl -k -H "Expect:" -T index.html https://<board-ip>/index.html
```

API it uses (see `../webapi.h`): `GET /api/status`, `GET /api/leds?v=<n>`,
`GET /api/seg?v=<n>`. The SD file listing is at `/?list` (the root `index.html`
otherwise masks it).
