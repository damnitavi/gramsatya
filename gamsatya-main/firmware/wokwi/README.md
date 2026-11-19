# Showing the hardware in Wokwi

Wokwi simulates a real ESP32 in the browser, including Wi-Fi with internet access
(`Wokwi-GUEST`, open network). So the sensor node can sign a reading with
HMAC-SHA256 and POST it to your running GramSatya app — the same code path as the
physical board. Nothing about the app has to change.

Two limits to know before you demo:

- **It cannot reach `localhost`.** The simulation runs in Wokwi's cloud, so it needs a
  public URL. One command gives you one (below).
- **No LoRa and no YF-S201 in Wokwi.** The ultrasonic tank sensor is exact; flow and
  lamp load are driven by a potentiometer instead of pulse counting. The real
  firmware in `../esp32_node/esp32_node.ino` keeps the pulse path for actual hardware.

## 1. Start the app on a fixed port

```bash
npm run dev -- -p 3000
```

## 2. Give it a public URL

```bash
npm run tunnel
```

It prints a box containing `https://something-random.trycloudflare.com`. Your endpoint
is that URL plus `/api/ingest`. Keep the terminal open while you demo — the URL changes
every time you restart it.

(Cloudflare's quick tunnel needs no account. If it exits with a download error, run it
once with the app stopped to let it fetch its binary, then try again.)

## 3. Open Wokwi

1. Go to <https://wokwi.com/projects/new/esp32> (no account needed to run).
2. Open the `diagram.json` tab, select all, delete, and paste in `diagram.json` from
   this folder.
3. Open `sketch.ino`, replace everything with `sketch.ino` from this folder.
4. On line ~39 set `SERVER_URL` to `https://something-random.trycloudflare.com/api/ingest`, and pick the
   sensor with `SENSOR_KIND` / `SENSOR_ID`:

   | `SENSOR_KIND` | `SENSOR_ID` | Part you drive |
   |---|---|---|
   | `KIND_HANDPUMP` | `radhaganj-handpump-1` | potentiometer (L/min) |
   | `KIND_STREETLIGHT` | `radhaganj-streetlight-2` | potentiometer (watts) |
   | `KIND_TANK` | `haripur-char-tank-1` | HC-SR04 distance slider |
   | `KIND_BIN` | `radhaganj-bin-1` | HC-SR04 distance slider |

5. Press the green ▶ button.

## What the judges see

- Serial monitor prints `POST 200 {"ok":true,"verified":true,...}` every 10 s.
- The green LED flashes on every accepted signature.
- In the app, open **Devices**: the sensor flips to `ESP32 signed` within a few seconds
  (a verified device reading overrides the simulated value for 120 s).
- Turn the pot to 0 and the pump/lamp shows as broken in the scorecard; turn it back to
  ~12 L/min (or ~18 W) and it recovers.
- Set `FORGE_READING = true`, re-run, and the server answers `401 signature check
  failed` — the LED blinks three times. That is the anti-fake-data story.

## If you would rather not use a tunnel

Use the **Wokwi for VS Code** extension: its private IoT gateway can reach your laptop,
so the sketch can talk to `http://host.wokwi.internal:3000/api/ingest` with no public
URL. You still need a local compile (`arduino-cli`, board `esp32:esp32:esp32`) for the
extension to load the firmware.

## If you have no time at all

`npm run dev`, open **Devices** in the app, click **Start virtual device**. That is the
built-in virtual ESP32 — same signed-reading path, no Wokwi, no tunnel.
