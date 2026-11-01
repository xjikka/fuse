# exthw – výstup na reálný čip AY (XJ extmod)

Každý zápis do registru AY ve FUSE (`sound_ay_write`) se pošle i ven:

| režim | cíl | formát |
|---|---|---|
| `udp` | `exthwudphost:exthwudpport` (AYSender) | 2 B `[reg, val]`, jen R0–R13; reg 0xFF = log (1 start, 21 init zvuku, 255 konec), 0xFE = cmd |
| `com` | `exthwcomport` @ `exthwcombaud` (Arduino `AY_PlayerFuse.ino`) | 3 B `'w' reg val`, jen R0–R13; při startu `'r' 0 0` (reset AY). Log/cmd se neposílají. |

Při pauze a ukončení se pošle R8–R10 = 0 (umlčení).

## Časovaný režim (protokol v2, `exthwtimed=1`)

Emulace neběží rovnoměrně: snímky se počítají v dávkách podle časovače Windows (rozestupy 16 a 31 ms),
takže okamžité posílání rozhodí časování (kolísání tempa, nefunkční digitalizovaný zvuk). V časovaném
režimu FUSE sbírá zápisy celého snímku s časem v T-stavech a pošle je najednou. Zařízení je přehraje
s pevným zpožděním `exthwlatency` podle vlastního časovače. Stejné bajty jdou po COM i jako jeden UDP
datagram.

| příkaz | bajty | význam |
|---|---|---|
| konfigurace | `'C'` cpu_hz(u32 LE) frame_t(u32 LE) latency_ms(u8) | při startu a při změně stroje nebo zpoždění |
| snímek | `'F'` seq(u8) len(u16 LE) data[len] xor(u8) | zápisy jednoho snímku (posílá se i prázdný) |
| flush+mute | `'X'` 0 0 | vyprázdnit buffer, ztlumit (pauza, reset, konec) |

Položka v `data`: `reg`, `val`, `dt` (varint LEB128: T-stavy od předchozí položky, u první od začátku
snímku). Typicky 3 B na zápis. `xor` je přes `data`. Starý `'r' 0 0` při otevření COM zůstává.

Vyžaduje firmware `AY_PlayerFuseTimed`. **Se starým `AY_PlayerFuse` nastavit `exthwtimed=0`**, jinak
bajty paketů mohou starý firmware zmást. Na Nano (2 KB RAM) spolehlivě hudba a slabý digitalizovaný
zvuk (řádově do 8 000 zápisů/s); při plném bufferu firmware zahazuje jednotlivé zápisy a jejich čas
přičte k dalšímu. Zpoždění 20 ms na Windows nestačí (dávky chodí až po 31 ms), doporučeno 40 ms.
Návrh: `exthw/doc/plan_20261101_1352_timed_ay.md`. Firmware a výsledky simulace:
`d:\_DataPrg\Arduino\projects\AY_PlayerFuseTimed\README.md`.

## Nastavení

`fuse.cfg` vedle `fuse.exe` (portable režim), jinak `%USERPROFILE%\fuse.cfg`.
XML; FUSE ho přepíše při ukončení (autosave) nebo při „Save options". Upravovat jen při zavřeném FUSE.

```xml
<exthwmode>com</exthwmode>            <!-- none | udp | com -->
<exthwudphost>127.0.0.1</exthwudphost>
<exthwudpport>9101</exthwudpport>
<exthwcomport>COM3</exthwcomport>
<exthwcombaud>2000000</exthwcombaud>
<exthwmuteinternal>1</exthwmuteinternal> <!-- 1 = při mode != none ztlumit interní AY -->
<exthwtimed>1</exthwtimed>             <!-- 1 = protokol v2 (firmware AY_PlayerFuseTimed) -->
<exthwlatency>40 ms</exthwlatency>     <!-- 20 ms | 40 ms | 60 ms -->
```

Všechno jde nastavit i v dialogu **Options → Peripherals → Real AY chip...**.

Nebo z příkazové řádky: `--exthw-mode com --exthw-com-port COM3 --exthw-com-baud 2000000`,
`--exthw-mode udp --exthw-udp-host 192.168.1.10 --exthw-udp-port 9101`, `--no-exthw-mute-internal`, `--no-exthw-timed`, `--exthw-latency "20 ms"`.

Výchozí `exthwmode` je `none` (čistý FUSE). Když se port nepodaří otevřít, FUSE ohlásí chybu
a běží dál bez externího výstupu.

Pozn.: otevření COM na Arduino Nano ho přes DTR resetuje (~1–2 s bootloader); prvních pár snímků
se ztratí, FUSE ale posílá všech 14 registrů každý snímek, takže se stav hned dorovná.
