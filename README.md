Removed. Below is the **cleaned README** with the **entire “Important Notice” / `$USER` section deleted**, nothing else changed.

---

# linux-omen-module

## Blacklist the Stock `hp-wmi` Driver

Before using this module, **blacklist the currently running `hp-wmi` kernel driver** to avoid conflicts.

Create a blacklist file:

```bash
echo "blacklist hp_wmi" | sudo tee /etc/modprobe.d/blacklist-hp_wmi.conf
```

Then reboot your system.

---

## Build the Module

1. Build the kernel module:

   ```bash
   make
   ```

2. After a successful build, copy the generated module file:

   ```bash
   hpwmi.ko
   ```

   to the appropriate kernel module directory as described in the documentation or installation guide.

3. Copy the provided **binaries and systemd unit files** to the locations specified in their respective folders.

---

## Fan Control

### Max Fan Mode

Fan max mode is controlled directly via **sysfs**.

* **Enable Max Fan Mode**

  ```bash
  echo 0 | sudo tee /sys/devices/platform/hp-wmi/hwmon/hwmon*/pwm1_enable > /dev/null
  ```

* **Restore Default (Automatic) Fan Control**

  ```bash
  echo 1 | sudo tee /sys/devices/platform/hp-wmi/hwmon/hwmon*/pwm1_enable > /dev/null
  ```

---

### Manual Fan Speed Control

Manual fan speed control is performed via **sysfs**.

* Set fan speed using a hexadecimal value:

  ```bash
  echo "$hex_value" | sudo tee /sys/devices/platform/hp-wmi/fanspeed > /dev/null
  ```

Example:

```bash
hex_value=0x32
echo "$hex_value" | sudo tee /sys/devices/platform/hp-wmi/fanspeed > /dev/null
```

---

## GPU MUX Control

Some supported devices expose a **GPU MUX switch** via sysfs.

The MUX has **three modes**, starting from `0`:

| Value | Mode                 |
| ----: | -------------------- |
|   `0` | Hybrid (iGPU + dGPU) |
|   `1` | Discrete GPU only    |
|   `2` | optimus GPU mode  |

* Set the MUX mode:

  ```bash
  echo 1 | sudo tee /sys/devices/platform/hp-wmi/mux > /dev/null
  ```

---

## Additional Features

* **Keyboard Backlight Support**
  Available for devices that support it.
  Identifier: `[8BCD]`

* **Performance Modes**
  Performance profiles are exposed through **`platform-profiles`** and can be controlled using standard Linux tooling.

---

## Notes

This project ships with **helper binaries and systemd service/timer units**.
They are intentionally **simple and easy to read**, allowing users to audit, modify, or extend them as needed.

---

If you want to tighten this further (shorter README, more “man page” style, or add systemd unit examples inline), say the word.
