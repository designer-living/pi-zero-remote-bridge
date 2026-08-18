## Pairing a Bluetooth remote

Ensure you have bluetooth installed (on DietPi in `dietpi-config` select `Advanced Options` -> `Bluetooth`)

You need to run:
```
$ sudo bluetoothctl
[bluetooth]# agent on
[bluetooth]# default-agent
[bluetooth]# scan on
[bluetooth]# pair <MAC ADDRESS FOUND>
[bluetooth]# connect <MAC ADDRESS FOUND>
[Chromecast Remote]# trust <MAC ADDRESS FOUND>
```

e.g.

```
$ sudo bluetoothctl
[bluetooth]# agent on
Agent is already registered
[bluetooth]# default-agent
Default agent request successful

[bluetooth]# scan on
Discovery started
[CHG] Controller B8:27:EB:86:5D:84 Discovering: yes
[CHG] Device A6:B4:38:63:78:BB Name: Chromecast Remote


[bluetooth]# pair A6:B4:38:63:78:BB
Attempting to pair with A6:B4:38:63:78:BB
[CHG] Device A6:B4:38:63:78:BB Connected: yes
Failed to pair: org.bluez.Error.ConnectionAttemptFailed
[CHG] Device A6:B4:38:63:78:BB Connected: no
[bluetooth]# connect A6:B4:38:63:78:BB
Attempting to connect to A6:B4:38:63:78:BB
[CHG] Device A6:B4:38:63:78:BB Connected: yes
[Chromecast Remote]# trust A6:B4:38:63:78:BB
[CHG] Device A6:B4:38:63:78:BB Trusted: yes
Changing A6:B4:38:63:78:BB trust succeeded
```

To test it worked run evtest and you should see the remote listed:

```
$ sudo evtest
No device specified, trying to scan all of /dev/input/event*
Available devices:
/dev/input/event4:      Chromecast Remote
```

### Pairing mode Buttons

* *Amazon Fire TV Remote* - Hold the 'home' button for 10 seconds. If you need to reset it first hold 'left', 'back' and 'menu' (3 lines) for 12 seconds. Then remove the batteries and put them back in.
* *Google Chromecast Remote* - Press and hold the 'Back' and 'Home' buttons on the remote until the light on the remote starts to pulse.
* *Humax Aura Remote* - Hold the 'i' and 'OK' buttons for 5 seconds
