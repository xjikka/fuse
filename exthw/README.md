# exthw – výstup na reálný čip AY (XJ extmod)

Každý zápis do registru AY ve FUSE (`sound_ay_write`) se pošle i ven:

| režim | cíl | formát |
|---|---|---|
| `udp` | `exthwudphost:exthwudpport` (AYSender) | 2 B `[reg, val]`, jen R0–R13; reg 0xFF = log (1 start, 21 init zvuku, 255 konec), 0xFE = cmd |
| `com` | `exthwcomport` @ `exthwcombaud` (Arduino `AY_PlayerFuse.ino`) | 3 B `'w' reg val`, jen R0–R13; při startu `'r' 0 0` (reset AY). Log/cmd se neposílají. |

Při pauze a ukončení se pošle R8–R10 = 0 (umlčení).

## Nastavení

`%USERPROFILE%\fuse.cfg` (XML, FUSE ho přepíše při „Save options"):

```xml
<exthwmode>com</exthwmode>            <!-- none | udp | com -->
<exthwudphost>127.0.0.1</exthwudphost>
<exthwudpport>9101</exthwudpport>
<exthwcomport>COM3</exthwcomport>
<exthwcombaud>2000000</exthwcombaud>
<exthwmuteinternal>1</exthwmuteinternal> <!-- 1 = při mode != none ztlumit interní AY -->
```

Nebo z příkazové řádky: `--exthw-mode com --exthw-com-port COM3 --exthw-com-baud 2000000`,
`--exthw-mode udp --exthw-udp-host 192.168.1.10 --exthw-udp-port 9101`, `--no-exthw-mute-internal`.

Výchozí `exthwmode` je `none` (čistý FUSE). Když se port nepodaří otevřít, FUSE ohlásí chybu
a běží dál bez externího výstupu.

Pozn.: otevření COM na Arduino Nano ho přes DTR resetuje (~1–2 s bootloader); prvních pár snímků
se ztratí, FUSE ale posílá všech 14 registrů každý snímek, takže se stav hned dorovná.
