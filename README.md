# linux-omen-module

backlist the current running hp_wmi driver

## Build the Module

1. Build the module.

## Note

Max fan set works for 120 seconds.  
This can be done by changing the value to `0` in `pwm1_enable` using the following command:

```bash
sudo echo 0 | sudo tee /sys/class/hwmon/hwmon*/pwm1_enable
```
To turn it off, set the value to 2:

```bash
sudo echo 2 | sudo tee /sys/class/hwmon/hwmon7/pwm1_enable
```

The backlight feature has been added for devices that support it.

This adds the note about thermal profiles not being set.
