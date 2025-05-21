# linux-omen-module

blacklist the current running hp_wmi driver

## Build the Module

1. Build the module:
   ```bash
   make 
   ```

2. After building, copy the module file (hp_wmi.ko) to the appropriate location, as mentioned in the documentation or installation guide.

3. Also copy the executables on the locations as mentioned in the folder

Note

BIOS protection is 120 seconds — so OmenHsa must be called every 120 seconds to maintain fan settings.
A timer function has been added for this; you can check how it works in the scripts

Max fan set works for 120 seconds.
This can be done using the application called fan_max using the following command:
```bash
fan_max 0 - Turns off max fan mode
fan_max 1 - Enables max fan mode
```
For manual fan speed control, there is also an application called fan_speed which takes a hexadecimal value as input:

```bash
fan_speed [hex_value]
```

The backlight feature has been added for devices that support it.[8BCD]

Performance modes are now available and can be controlled via platform-profiles.

"⚠️ Important: If any of the provided executables or scripts contain references to $USER, please verify that it matches your system username. If necessary, replace $USER with your actual username to avoid runtime issues. "

Also, feel free to read through the executables/scripts — they are simple and easy to understand.
