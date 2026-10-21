# ESP32-S3 Delta OTA over A7672S (UART / PPP)

Step-by-step project to test **compressed Delta OTA** on ESP32-S3 using a **SIMCom A7672S** 4G modem over UART, based on Espressif's [`https_delta_ota`](https://github.com/espressif/idf-extra-components/tree/master/esp_delta_ota/examples/https_delta_ota) example.

```
A7672S 4G Modem
      │
    UART (RX/TX)
      │
      ▼
  ESP32-S3
      │
    PPP (ATD*99#)
      │
      ▼
 ESP-NETIF / Internet
      │
    HTTPS
      │
      ▼
 OTA Server (patch.bin)
      │
  esp_delta_ota
      │
 V1 + Patch → V2 in ota_1
      │
    Reboot
      ▼
 Updated ESP32-S3
```

---

## 1. Hardware wiring (A7672S ↔ ESP32-S3)

| Signal | ESP32-S3 | A7672S module |
|--------|----------|---------------|
| TX | **GPIO17** | **RXD** |
| RX | **GPIO18** | **TXD** |
| PWRKEY | **GPIO4** | PWRKEY (pulse LOW ~1.5 s to power on) |
| DTR | **GPIO21** | DTR (drive **LOW** — do not leave floating) |
| GND | GND | GND |
| Power | — | VBAT **3.4–4.2 V** (typ **3.8 V**), peak **≥ 1 A**, bulk cap **≥ 200 µF** |

Insert an active LTE SIM with a data plan. Attach the LTE antenna.

Pin defaults live in `main/config.h`. Change them only if your board uses different GPIOs.

---

## 2. Prerequisites

| Item | Version / note |
|------|----------------|
| ESP-IDF | **≥ 5.3** (tested with 5.3.1) |
| Target | `esp32s3` |
| Python (host) | For patch tool: `pip install detools` |
| OTA server | HTTPS host reachable over the cellular APN |

---

## 3. Create / open this project

This folder is a ready-made ESP-IDF project. If you need to recreate it from the upstream example:

```powershell
cd T:\ESP32_PROJECTS
idf.py create-project-from-example "espressif/esp_delta_ota=1.1.4:https_delta_ota" "D FOTA"
```

Then replace Wi-Fi `example_connect()` with the A7672S PPP code from this tree (`modem.c`, `ppp_manager.c`, `main.c`).

---

## 4. Required ESP-IDF components

Declared in `main/idf_component.yml`:

| Component | Purpose |
|-----------|---------|
| `espressif/esp_modem` | UART DTE/DCE, `ATD*99#` PPP dial |
| `espressif/esp_delta_ota` | Apply compressed delta patch |
| (IDF built-in) `esp_http_client` | HTTPS download over PPP |
| (IDF built-in) `app_update` / `esp_ota_ops` | Dual OTA partitions |
| (IDF built-in) `esp_netif` + lwIP PPP | IP stack on cellular |

---

## 5. `menuconfig` settings

```powershell
cd "T:\ESP32_PROJECTS\D FOTA"
idf.py set-target esp32s3
idf.py menuconfig
```

### Component config → lwIP

These are pre-set in `sdkconfig.defaults`:

- `CONFIG_LWIP_PPP_SUPPORT=y`
- `CONFIG_LWIP_PPP_PAP_SUPPORT=y`
- `CONFIG_LWIP_PPP_CHAP_SUPPORT=y`

### Partition Table

- **Custom partition table** → `partitions.csv` (dual OTA: `ota_0` + `ota_1`, 1.25 MB each)

### Delta OTA Configuration

| Option | Typical value |
|--------|---------------|
| **Delta patch HTTPS URL** | `https://YOUR_SERVER:8070/patch.bin` |
| **HTTP read timeout** | `30000` ms (cellular is slow) |
| **Skip TLS CN check** | `y` for self-signed local server |
| **Use certificate bundle** | `n` for local server; `y` for public CA (AWS, etc.) |
| **Auto start OTA** | `y` |
| **Start delay** | `5` s after PPP up |

### Modem / A7672S Configuration

| Option | Example |
|--------|---------|
| **SIM APN** | `airtelgprs.com`, `jionet`, `internet`, carrier-specific |

---

## 6. Build V1 firmware (base)

1. Edit root `CMakeLists.txt`:

```cmake
set(PROJECT_VER "v1.0.0")
```

2. Build and save the binary:

```powershell
cd "T:\ESP32_PROJECTS\D FOTA"
idf.py build
Copy-Item build\delta_fota.bin firmware\v1.bin
```

3. Flash V1 to the device:

```powershell
idf.py -p COMx flash monitor
```

Expected serial log:

```
APP: ESP32-S3 Delta OTA over A7672S PPP
APP: Firmware : v1.0.0
MODEM: CEREG: Registered ...
PPP: IP : 10.x.x.x
APP: PPP online — DNS/TCP OK
```

---

## 7. Build V2 firmware (target)

1. Make a visible change (e.g. add a log line in `main.c`).
2. Update version:

```cmake
set(PROJECT_VER "v2.0.0")
```

3. Build and save:

```powershell
idf.py build
Copy-Item build\delta_fota.bin firmware\v2.bin
```

**Do not flash V2 yet.** Keep **V1** running on the board.

---

## 8. Generate the delta patch (host PC)

Install the patch tool (once):

```powershell
pip install detools
```

Or, after first `idf.py build`, use the copy inside managed components:

```powershell
pip install -r managed_components\espressif__esp_delta_ota\examples\https_delta_ota\tools\requirements.txt
```

Create the patch (**base must match exactly what is running on the device**):

```powershell
python managed_components\espressif__esp_delta_ota\tools\esp_delta_ota_patch_gen.py create_patch `
  --chip esp32s3 `
  --base_binary firmware\v1.bin `
  --new_binary firmware\v2.bin `
  --patch_file_name firmware\patch.bin
```

Verify before hosting:

```powershell
python managed_components\espressif__esp_delta_ota\tools\esp_delta_ota_patch_gen.py verify_patch `
  --chip esp32s3 `
  --base_binary firmware\v1.bin `
  --new_binary firmware\v2.bin `
  --patch_file_name firmware\patch.bin
```

> **Important:** The patch embeds a SHA-256 of the **running** firmware. If you rebuild V1 with different options and flash a different binary, patch header verification will fail with `Patch SHA256 does not match running firmware`.

---

## 9. Host the patch on an HTTPS server

### Option A — Simple Python HTTPS server (local / lab)

Generate a self-signed cert (once):

```powershell
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 3650 -nodes -subj "/CN=192.168.1.100"
```

Copy `server.crt` into `main/server_cert.pem`, rebuild, and set menuconfig URL to your server.

Serve the patch:

```powershell
# Example using ESP-IDF's simple HTTPS server or any static HTTPS host
# Place firmware\patch.bin where your server can serve it as /patch.bin
```

### Option B — Public cloud URL

- Upload `patch.bin` to S3 / GitHub Releases / your CDN.
- Set `CONFIG_DELTA_OTA_USE_CERT_BUNDLE=y` in menuconfig.
- Set patch URL to the public HTTPS link.

---

## 10. Run the Delta OTA test

1. Device is running **V1** (`v1.0.0` in boot log).
2. Patch is hosted at the URL in menuconfig.
3. Rebuild if you changed URL/cert (same V1 can stay flashed if only URL changed).
4. Reset the board (or flash same V1 again).

Watch the monitor:

```
DELTA_OTA: Running firmware: version=v1.0.0
DELTA_OTA: Patch URL: https://...
DELTA_OTA: HTTP status=200
DELTA_OTA: Source partition : ota_0
DELTA_OTA: Target partition : ota_1
DELTA_OTA: Download complete
DELTA_OTA: Delta OTA succeeded — boot partition set to ota_1
DELTA_OTA: Rebooting in 5 s...
```

After reboot:

```
APP: Firmware : v2.0.0
```

---

## 11. Verify Delta OTA succeeded

| Check | How |
|-------|-----|
| **Version string** | Boot log shows `Firmware : v2.0.0` |
| **Partition swap** | Log shows boot from `ota_1` (was `ota_0` before OTA) |
| **Functional change** | Your V2-specific log/behavior appears |
| **No patch errors** | No `Patch SHA256 does not match` or `Patch apply failed` |
| **Optional** | `esp_ota_get_running_partition()->label` in code |

Rollback: flash `v1.bin` again to the inactive partition or full reflash.

---

## 12. How PPP + ESP-NETIF fits together

1. **`modem_init()`** — Creates UART DTE, SIM7600-compatible DCE (A7672S AT set), and a PPP `esp_netif` via `ESP_NETIF_DEFAULT_PPP()`.
2. **`modem_run_state_machine()`** — AT sync, SIM, CEREG, `AT+CGDCONT=1,"IP","<apn>"`. **Never** `AT+CGACT` on LTE cid1 (A7672S restriction).
3. **`ppp_manager_connect()`** — `esp_modem_set_mode(DATA)` → modem sends **`ATD*99#`** (true PPP). Waits for `IP_EVENT_PPP_GOT_IP`.
4. **Default netif** — Once PPP has an IP, `esp_http_client` uses the cellular route automatically.
5. **`delta_ota_run()`** — HTTPS GET patch → verify 64-byte header (magic + SHA256 of running app) → stream patch through `esp_delta_ota_feed_patch()` → write reconstructed V2 to `ota_1` → `esp_ota_set_boot_partition()` → reboot.

---

## 13. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `AT test FAILED` | Wiring / power | Check GPIO17/18, VBAT 3.8 V, bulk cap, PWRKEY pulse |
| `PPPERR_LOOPBACK` | Wrong dial command | Must use `ATD*99#`, not `AT+CGDATA` |
| `HTTP open failed` | No route / DNS | Confirm PPP IP; test with `ppp_manager_verify_connectivity` |
| TLS errors | Cert mismatch | Update `server_cert.pem` or enable cert bundle |
| `Patch SHA256 does not match` | Wrong base binary | Regenerate patch from exact running `v1.bin` |
| `Patch apply failed` | Corrupt download or wrong chip | Re-verify patch; confirm `--chip esp32s3` |
| OTA works on Wi-Fi but not LTE | Timeout too short | Increase `CONFIG_DELTA_OTA_RECV_TIMEOUT_MS` |

---

## 14. Project layout

```
D FOTA/
├── CMakeLists.txt          # PROJECT_VER = v1.0.0 or v2.0.0
├── partitions.csv          # ota_0 + ota_1
├── sdkconfig.defaults      # esp32s3, PPP, TLS
├── README.md               # this file
└── main/
    ├── main.c              # PPP bring-up + trigger delta OTA
    ├── delta_ota.c         # https_delta_ota logic
    ├── modem.c / modem.h   # A7672S AT stages 1–4
    ├── ppp_manager.c       # ATD*99# + GOT_IP
    ├── config.h            # GPIO / timeouts
    ├── Kconfig.projbuild   # APN, patch URL
    ├── idf_component.yml   # esp_modem + esp_delta_ota
    └── server_cert.pem     # TLS trust anchor for OTA server
```

---

## 15. Quick command cheat sheet

```powershell
# Setup
cd "T:\ESP32_PROJECTS\D FOTA"
idf.py set-target esp32s3
idf.py menuconfig

# V1
# set PROJECT_VER "v1.0.0" in CMakeLists.txt
idf.py build flash monitor

# V2 binary for patch only
# set PROJECT_VER "v2.0.0"
idf.py build

# Patch
python managed_components\espressif__esp_delta_ota\tools\esp_delta_ota_patch_gen.py create_patch --chip esp32s3 --base_binary firmware\v1.bin --new_binary firmware\v2.bin --patch_file_name firmware\patch.bin

# Test OTA (V1 flashed, patch on server)
idf.py -p COMx monitor
```

---

## References

- [esp_delta_ota component](https://github.com/espressif/idf-extra-components/tree/master/esp_delta_ota)
- [https_delta_ota example](https://github.com/espressif/idf-extra-components/tree/master/esp_delta_ota/examples/https_delta_ota)
- [esp_modem pppos_client](https://github.com/espressif/esp-protocols/tree/master/components/esp_modem/examples/pppos_client)
- Working A7672S PPP foundation: `T:\shaik\fota` (Phase 0)
