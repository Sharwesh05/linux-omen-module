# linux-omen-module

This repository ships **two DKMS kernel modules** for HP OMEN laptops (DMI
board name **`8BCD`**):

| Module    | Purpose                                                                 |
| --------- | ----------------------------------------------------------------------- |
| `hpomen`  | Drop-in replacement for the stock `hp_wmi` driver (fans, MUX, hotkeys, platform profiles, etc.). |
| `acpi_ec` | Out-of-tree EC access driver. Used **only** when the in-tree `ec_sys` module is not available. |

Helper binaries in `bin/` (e.g. `fan_speed`, `fan_max`, `ec_read`, `Omenfan`,
`Omenhsa`) are intended to be installed into `/usr/local/bin/` so they can be
called directly from the shell or from the supplied systemd units.

---

## 1. Blacklist `hp_wmi`

`hpomen` clashes with the stock `hp_wmi` driver, so the latter has to be
blacklisted before `hpomen` can take over.

```bash
echo "blacklist hp_wmi" | sudo tee /etc/modprobe.d/blacklist-hp_wmi.conf
sudo update-initramfs -u   # Debian/Ubuntu
# or
sudo dracut -f             # Fedora/Arch
```

Reboot afterwards.

---

## 2. Install the DKMS modules

Both modules are DKMS-ready. Place the source trees somewhere persistent
(e.g. `/usr/src/`) and register them with DKMS.

```bash
sudo cp -r hpomen-1.0   /usr/src/
sudo cp -r acpi_ec-1.0  /usr/src/

sudo dkms add    hpomen/1.0
sudo dkms add    acpi_ec/1.0

sudo dkms build  hpomen/1.0
sudo dkms build  acpi_ec/1.0

sudo dkms install hpomen/1.0
sudo dkms install acpi_ec/1.0
```

### Loading rules

* **`hpomen`** replaces `hp_wmi` and should be loaded at startup:

  ```bash
  echo "hpomen" | sudo tee /etc/modules-load.d/hpomen.conf
  ```

* **`acpi_ec`** must only be loaded when the in-tree `ec_sys` module is
  **not** present. Use a soft-dependency so DKMS / modprobe picks the right
  one:

  ```bash
  echo "softdep acpi_ec pre: ec_sys" | sudo tee /etc/modprobe.d/acpi_ec.conf
  ```

  In practice: if your kernel already exposes `ec_sys` (`/sys/kernel/debug/ec/ec0/io`),
  keep using `ec_sys`; only fall back to `acpi_ec` on kernels/distros where
  `ec_sys` is missing.

---

## 3. Install the helper binaries

The `bin/` directory contains executable scripts used by the helper
systemd units and for manual fan control. Copy everything to
`/usr/local/bin/` so they live on the default `PATH`:

```bash
sudo install -m 0755 bin/fan_max   /usr/local/bin/
sudo install -m 0755 bin/fan_speed /usr/local/bin/
sudo install -m 0755 bin/ec_read   /usr/local/bin/
sudo install -m 0755 bin/Omenfan   /usr/local/bin/
sudo install -m 0755 bin/Omenhsa   /usr/local/bin/
```

### What each binary does

| Binary       | Type    | Function |
| ------------ | ------- | -------- |
| `fan_speed`  | bash    | Write a hexadecimal fan-speed value to `/sys/devices/platform/hp-wmi/fanspeed` (e.g. `fan_speed 0x32`). |
| `fan_max`    | bash    | Toggle max fan mode via `pwm1_enable` (`1` → max, stops `Omenfan.service`; `0` → automatic, starts it). |
| `ec_read`    | python  | Dump the 256-byte EC space; auto-detects `ec_sys` (`/sys/kernel/debug/ec/ec0/io`) vs `acpi_ec` (`/dev/ec`). |
| `Omenfan`    | python  | Closed-loop daemon: reads CPU/GPU temps via EC and adjusts the fan via `fan_speed`. |
| `Omenhsa`    | bash    | One-shot poke at `/sys/devices/platform/hp-wmi/fancount`; driven by `Omenhsaclient.timer`. |

---

## 4. Systemd units (required for automatic fan / thermal profile)

The units in `etc/systemd/system/` **must** be installed into the global
systemd location (`/etc/systemd/system/`). They are not optional:

| Unit file                  | Purpose                                                                                  |
| -------------------------- | ---------------------------------------------------------------------------------------- |
| `Omenfan.service`          | Runs the `Omenfan` daemon so the fan is controlled automatically.                        |
| `Omenhsaclient.service`    | One-shot helper invoked by the timer to poke the thermal-profile / HSA interface.        |
| `Omenhsaclient.timer`      | Schedules `Omenhsaclient.service` so the thermal profile is refreshed at startup / periodically. |

Install and activate them:

```bash
sudo cp etc/systemd/system/Omenfan.service        /etc/systemd/system/
sudo cp etc/systemd/system/Omenhsaclient.service /etc/systemd/system/
sudo cp etc/systemd/system/Omenhsaclient.timer   /etc/systemd/system/

sudo systemctl daemon-reload

# Start the fan auto-control daemon now + on boot
sudo systemctl enable --now Omenfan.service

# Start the thermal-profile scheduler now + on boot
sudo systemctl enable --now Omenhsaclient.timer
```

Verify with:

```bash
systemctl status Omenfan.service Omenhsaclient.timer --no-pager
systemctl list-timers Omenhsaclient.timer --no-pager
```

---

## 5. Manual fan control (examples)

```bash
# Set a fixed fan speed (hex)
fan_speed 0x32

# Force maximum fan speed and stop the auto daemon
fan_max 1

# Hand control back to Omenfan
fan_max 0

# Inspect the raw EC contents
ec_read
```

---

## Supported hardware

Built and tested against HP OMEN laptops whose DMI board name is **`8BCD`**
(defined in `hpomen-1.0/hpomen.c` as `thermal_profile_v1_boards`).