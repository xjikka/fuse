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

Položka v `data`:
- plná: `reg` (0–13), `val`, `dt` (varint LEB128: T-stavy od předchozí položky, u první od začátku
  snímku), typicky 3–4 B;
- krátká, jen pro stejný registr jako předchozí položka téhož snímku a `val` < 32 (digi přes hlasitost):
  `0x40|val` + int8 (dt − předchozí dt), nebo `0x60|val` při stejném dt. 1–2 B.

R8–R10 se posílají jen 5 bitů (víc čip nedrží). Snímek má nejvýš `exthwmaxframe` bajtů (Nano 1024 B,
Pico 16000 B), další zápisy toho snímku se vynechají (čas se přičte k dalšímu). `xor` je přes `data`. Starý `'r' 0 0` při otevření COM zůstává.

Vyžaduje firmware `AY_PlayerFuseTimed` a **COM 500 000 Bd** (`exthwcombaud`). Při 2 Mbaud by Nano
během dávky trávil skoro všechen čas v přerušení příjmu a zápisy by se opožďovaly; 115 200 Bd stačí
jen na hudbu.
**Se starým `AY_PlayerFuse` nastavit `exthwtimed=0`** (a jeho 2 000 000 Bd), jinak bajty paketů starý
firmware zmatou. Na Nano spolehlivě hudba i digi kolem 10 000 zápisů/s (Lyra II part 6: 9 750 zápisů/s,
391 B na snímek). Firmware přehrává jen snímky se správným `xor`, vadný snímek vynechá.
Zpoždění 20 ms na Windows nestačí (dávky chodí až po 31 ms), doporučeno 40 ms.
Návrh: `exthw/doc/plan_20261101_1352_timed_ay.md`. Firmware a výsledky simulace:
`d:\_DataPrg\Arduino\projects\AY_PlayerFuseTimed\README.md`.

### Diagnostika v `exthw_fw.log` (COM)

Mezi řádky `S …` z firmwaru (jednou za sekundu) zapisuje FUSE řádky `H <s> <text>`, kde `<s>` jsou
sekundy od otevření COM:

| text | význam |
|---|---|
| `emulation gap N ms` | mezi dvěma konci snímku uběhlo ≥ 40 ms: emulace stála (okno, menu, Windows) |
| `COM write N B took M ms, written W, queued Q B` | `WriteFile` trval ≥ 20 ms nebo nezapsal vše: zdržení v ovladači USB/COM nebo zařízení nestíhá |
| `queue full, dropped N B` | fronta 64 kB plná, blok (snímek) zahozen |
| `frame full, N writes skipped` | snímek narazil na `exthwmaxframe` |

Výpadek (`und` ve firmwaru roste) s `emulation gap` těsně před ním způsobil FUSE nebo Windows,
s `COM write … took` přenos.

## Nastavení

`fuse.cfg` vedle `fuse.exe` (portable režim), jinak `%USERPROFILE%\fuse.cfg`.
XML; FUSE ho přepíše při ukončení (autosave) nebo při „Save options". Upravovat jen při zavřeném FUSE.

```xml
<exthwmode>com</exthwmode>            <!-- none | udp | com -->
<exthwudphost>127.0.0.1</exthwudphost>
<exthwudpport>9101</exthwudpport>
<exthwcomport>COM3</exthwcomport>
<exthwcombaud>500000</exthwcombaud>  <!-- 2000000 se starým AY_PlayerFuse -->
<exthwmuteinternal>1</exthwmuteinternal> <!-- 1 = při mode != none ztlumit interní AY -->
<exthwtimed>1</exthwtimed>             <!-- 1 = protokol v2 (firmware AY_PlayerFuseTimed) -->
<exthwlatency>40 ms</exthwlatency>     <!-- 20 ms | 40 ms | 60 ms -->
<exthwmaxframe>1024 B (Nano)</exthwmaxframe> <!-- 16000 B (Pico): firmware na RP2040 -->
```

Všechno jde nastavit i v dialogu **Options → Peripherals → Real AY chip...**.

Nebo z příkazové řádky: `--exthw-mode com --exthw-com-port COM3 --exthw-com-baud 500000`,
`--exthw-mode udp --exthw-udp-host 192.168.1.10 --exthw-udp-port 9101`, `--no-exthw-mute-internal`, `--no-exthw-timed`, `--exthw-latency "20 ms"`, `--exthw-max-frame "16000 B (Pico)"`.

Výchozí `exthwmode` je `none` (čistý FUSE). Když se port nepodaří otevřít, FUSE ohlásí chybu
a běží dál bez externího výstupu.

Pozn.: otevření COM na Arduino Nano ho přes DTR resetuje (~1–2 s bootloader); prvních pár snímků
se ztratí, FUSE ale posílá všech 14 registrů každý snímek, takže se stav hned dorovná.
