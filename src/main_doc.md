# Das Gewär

Das Projekt "Das Gewähr" wurde ursprünglich von obias Moswitzer und John Ramsak initiiert. Später schlossen sich Karl Krumrei und Erik Praster dem Team an. Das Vorhaben wird freundlicherweise von Luca Hofbauer ([Autohaus Hofbauer](https://www.hofbauer.co.at/autohaus)) gesponsert.

Bei "Das Gewähr" handelt es sich um ein selbst entwickeltes Laser-Tag-System. Das Team baut die gesamte Hard- und Software auf Basis der folgenden Kernkomponenten auf:

- **Mikrocontroller**: ESP32-WROOM-32 (53.5 mm x 28.5 mm x 13.0 mm)

- **Sender (in der "Waffe")**: TSAL6200 Hochleistungs-Infrarot-Sendedioden

- **Empfänger (an der "Weste")**: TSOP38438 IR-Empfängermodule

Die passgenauen Gehäuse für die Elektronik werden mithilfe von 3D-Druckern gefertigt, um die Komponenten zu schützen und dem System ein professionelles Aussehen zu verleihen.

## Inhaltsverzeichnis
- [**Beschreibung**](#das-gewär)
- [**Inhaltsverzeichnis**](#inhaltsverzeichnis)
- [**Grundlegende Konzepte**](#grundlegende-konzepte)
    - [Die `setup()` und `loop()` Struktur](#grundlegende-konzepte)
    - [Statische Datentypen (z.B. `uint8_t`, `uint16_t`)](#grundlegende-konzepte)
- [**Hardware-Konfiguration (I/O Pins)**](#hardware-konfiguration-io-pins)
- [**Funktionsweise im Detail**](#funktionsweise-im-detail)
    - [Das Senden eines "Schusses" (Sender)](#das-senden-eines-schusses-ir-signal)
        - [Modulation: Die 38-kHz-Trägerfrequenz (PWM)](#pulsweitenmodulation-pwm-als-trägerfrequenz)
        - [Datenübertragung: Das NEC-Protokoll](#das-nec-protokoll)
    - [Das Empfangen eines "Treffers" (Empfänger)](#das-empfangen-eines-treffers)
        - [Effiziente Erfassung durch Interrupts](#interrupts-die-effiziente-lösung)
        - [Dekodierung in der Interrupt Service Routine (`handleReceivedIR`)](#dekodierung-des-signals-in-handlereceivedir)
    - [Verarbeitung des Treffers (Spiellogik in `processHit`)](#verarbeitung-des-treffers)
- [**Konfiguration & Anpassung**](#konfiguration--anpassung)
    - [Anpassen von `playerID` & `teamID`](#konfiguration--anpassung)
    - [Anpassen der Spiellogik](#konfiguration--anpassung)

## Grundlegende Konzepte

Die Arduino-Umgebung, die hier für den ESP32 verwendet wird, hat eine Struktur, die durch zwei Hauptfunktionen definiert wird: `setup()` und `loop()`.

- `void setup()`: dieser teil der Code wird **Genau Einmal** ausgefürht es wird verwendet um sahen zu initializieren.

- `void loop()`: das ist eine Endloss schleife das als die main loop dient hier wird die haupt logik ausgeführt.

Ein weiterer wichtiger konzept sind **Datentypen**. In C++ musst du den Typ jeder Variable explizit deklarieren, z.B. int, uint16_t oder bool. Dies ermöglicht dem Compiler, den Code viel effizienter für die Hardware zu optimieren.

- `uint16_t`: Ein vorzeichenloser 16-Bit-Integer (Wert von 0 bis 65.535). Wird hier für die `playerID` verwendet.

- `uint8_t`: Ein vorzeichenloser 8-Bit-Integer (Wert von 0 bis 255). Wird für die `teamID` verwendet.

## Hardware-Konfiguration (I/O Pins)

Dieser code deffiniert zuerst drei Konstanten, um die GPIO-Pins des ESP32 zu speichern

``` cpp
const int irLedPin = 4;      // GPIO für die Sende-LED (Waffe)
const int irReceiverPin = 5; // GPIO für den Empfänger (Weste)
const int triggerPin = 15;   // GPIO für den Abzug-Knopf
```

Die `void setup()`-Funktion deffiniert wie diese pins verwendet werden:

- `pinMode(irLedPin, OUTPUT);`
- `pinMode(irReceiverPin, INPUT);`
- `pinMode(triggerPin, INPUT_PULLUP);`

## Funktionsweise im Detail

### Das Senden eines "Schusses" (IR-Signal)

Ein einfaches Ein- und Ausschalten der IR-LED würde nicht funktionieren, da der Empfänger auf ein ganz bestimmtes Signal wartet. Um Störungen durch andere Lichtquellen (wie die Sonne oder Lampen) zu vermeiden, muss das IR-Signal **moduliert** werden.

#### Pulsweitenmodulation (PWM) als Trägerfrequenz

Der Code verwendet eine Technik namens Pulsweitenmodulation (PWM), um die IR-LED nicht einfach nur einzuschalten, sondern sie extrem schnell blinken zu lassen – genau in 38 kHz takt. Dies wird als Trägerfrequenz bezeichnet. Der IR-Empfänger ist speziell darauf ausgelegt, nur IR-Licht zu "sehen", das mit dieser Frequenz blinkt.

In `setup()` werden diese Zeilen dafür verwendet:

``` cpp
// Konfiguriere ESP32 LEDC PWM für 38kHz Modulation
ledcSetup(pwmChannel, carrierFrequency, 8);
ledcAttachPin(irLedPin, pwmChannel);
ledcWrite(pwmChannel, 0); // LED anfangs aus
```

- `ledcSetup(...)`: Konfiguriert einen PWM-Kanal (pwmChannel) mit der gewünschten Frequenz (carrierFrequency).

- `ledcAttachPin(...)`: Verbindet den irLedPin mit diesem konfigurierten PWM-Kanal.

#### Das NEC-Protokoll

Um Daten (wer hat geschossen?) zu übertragen, wird ein standardisiertes Protokoll namens NEC verwendet. Es definiert eine präzise Abfolge von Impulsen (LED an) und Pausen (LED aus). Stell es dir wie Morsecode vor, aber für Maschinen.

Die Konstanten am Anfang des Codes definieren die genauen Zeitdauern in Mikrosekunden für dieses Protokoll:

``` cpp
// NEC protocol timing constants (in microseconds)
const unsigned long NEC_LEADING_PULSE = 9000;
const unsigned long NEC_LEADING_SPACE = 4500;
const unsigned long NEC_PULSE = 560;
const unsigned long NEC_ZERO_SPACE = 560;
const unsigned long NEC_ONE_SPACE = 1690;
const unsigned long NEC_REPEAT_SPACE = 2250;
const unsigned long NEC_REPEAT_DELAY = 108000; // 108ms between repeats
```

Die Funktion `sendNEC(playerID, teamID)` setzt diese Regeln um:

1. **Startsignal**: Sendet einen langen Impuls (`NEC_LEADING_PULSE`) gefolgt von einer langen Pause (`NEC_LEADING_SPACE`), damit der Empfänger weiß: "Achtung, eine neue Nachricht beginnt!"

2. **Datenübertragung**: Sendet 32 Bits an Information. In diesem Code werden die `playerID` (16 Bit) und die `teamID` (8 Bit) gesendet. Zur Fehlererkennung wird auch eine invertierte Version der `teamID` (8 Bit) mitgesendet.

    - Eine logische 1 wird durch einen kurzen Impuls und eine lange Pause dargestellt (`NEC_ONE_SPACE`).

    - Eine logische 0 wird durch einen kurzen Impuls und eine kurze Pause dargestellt (`NEC_ZERO_SPACE`).

3. **Stoppsignal**: Ein letzter kurzer Impuls (`NEC_PULSE`) beendet die Übertragung.

Wenn der Spieler den Abzug (`triggerPin`) drückt, wird `digitalRead(triggerPin)` zu `LOW`. Daraufhin ruft die `loop()`-Funktion `sendIRSignal()` auf, welche wiederum `sendNEC()` ausführt und den "Schuss" abfeuert.

### Das Empfangen eines "Treffers"

Das Empfangen ist komplizierter als das Senden, da ein Treffer jederzeit eintreffen kann. Würde man in der `loop()`-Funktion ständig den Empfänger abfragen (`polling`), könnte man leicht einen sehr kurzen Impuls verpassen.

#### Interrupts: Die effiziente Lösung

Der Code verwendet einen **Interrupt**. Ein Interrupt ist ein Mechanismus, bei dem die Hardware die normale Ausführung des Codes (also die `loop()`-Funktion) unterbricht, um eine dringende Aufgabe sofort zu erledigen.

In `setup()` wird der Interrupt mit dieser Zeile eingerichtet:

```cpp
// Attach interrupt to the IR receiver pin
attachInterrupt(digitalPinToInterrupt(irReceiverPin), handleReceivedIR, CHANGE);
```

Das bedeutet: "Jedes Mal, wenn sich der Spannungszustand am `irReceiverPin` ändert (von `LOW` zu `HIGH` oder umgekehrt), halte sofort an, was du gerade tust, und führe die Funktion `handleReceivedIR()` aus."

Die Funktion `handleReceivedIR()` ist die **Interrupt Service Routine (ISR)**. Sie muss extrem schnell sein, da während ihrer Ausführung der Hauptcode blockiert ist.

 - `IRAM_ATTR`: Dieses Schlüsselwort vor der Funktion ist eine ESP32-spezifische Anweisung an den Compiler, diese Funktion im schnellen internen RAM (IRAM) zu speichern. Dies stellt sicher, dass sie mit minimaler Verzögerung ausgeführt werden kann.

 - `volatile`: Variablen, die sowohl in der ISR als auch in der `loop()`-Funktion verwendet werden (z.B. `messageReady`, `receivedData`), müssen als `volatile` deklariert werden. Dies weist den Compiler an, keine Optimierungen vorzunehmen, die davon ausgehen, dass sich der Wert der Variable nicht unerwartet ändern kann. Für den Compiler ist die Änderung durch einen Interrupt "unerwartet".

#### Dekodierung des Signals in `handleReceivedIR()`

Diese Funktion arbeitet wie eine Stoppuhr:

1. Sie misst die Zeit zwischen den Signalflanken (die Pausen zwischen den IR-Impulsen).

2. Sie vergleicht die gemessene Pausenlänge mit den im NEC-Protokoll definierten Zeiten.

3. Anhand der Pausenlänge entscheidet sie, ob eine `0`, eine `1` oder das Startsignal empfangen wurde.

4. Bit für Bit setzt sie die 32-Bit-Nachricht (`receivedData`) zusammen.

5. Wenn 32 Bits empfangen wurden, setzt sie die Variable `messageReady` auf `true` und beendet sich.

### Verarbeitung des Treffers

Zurück in der `loop()`-Funktion wird ständig geprüft:

``` cpp
if (messageReady) {
  // ... verarbeite die Daten
}
```

Wenn die ISR einen vollständigen Schuss empfangen hat, wird dieser Block ausgeführt:

1. `messageReady` wird auf `false` zurückgesetzt.

2. Die empfangenen 32-Bit-Rohdaten (`lastReceivedData`) werden mit bitweisen Operationen (`&`, `>>`) in `playerID` und `teamID` zerlegt.

3. Fehlerprüfung: Es wird geprüft, ob die empfangene `command_inv` tatsächlich das logische Gegenteil des `command` ist. Dies stellt sicher, dass die Daten nicht während der Übertragung beschädigt wurden.

4. Wenn die Daten gültig sind, wird die Funktion `processHit(address, command)` aufgerufen. Hier findet die eigentliche Spiellogik statt (z.B. "War es ein Gegner?", "Ziehe Lebenspunkte ab", etc.).

## Konfiguration & Anpassung

Um das Spiel anzupassen, musst du hauptsächlich die folgenden globalen Variablen am Anfang der Datei ändern:

``` cpp
// Game data - customize these for your players/teams
uint16_t playerID = 0x1234;  // 16-bit player ID
uint8_t teamID = 0x01;       // 8-bit team ID
```

- `playerID`: Eine eindeutige ID für jeden Spieler. Der Wert wird in Hexadezimal-Notation (`0x...`) angegeben. `0x1234` ist nur ein Beispiel. Jeder Spieler sollte hier einen anderen Wert haben.

- `teamID`: Die ID des Teams. Spieler im selben Team sollten hier denselben Wert haben (z.B. `0x01` für Team 1, `0x02` für Team 2).

Die Spiellogik selbst kann in der Funktion `processHit()` angepasst werden. Dort kannst du festlegen, was bei einem Treffer passiert (z.B. Leben abziehen, Soundeffekte abspielen, LEDs aufleuchten lassen).

```cpp
void processHit(uint16_t shooterID, uint8_t shooterTeam) {
  // Eigene Spiellogik hier einfügen
  if (shooterTeam == teamID) {
    Serial.println("Friendly fire! No damage.");
  } else {
    Serial.println("Enemy hit! -10 health");
    // Hier Code einfügen, um Leben abzuziehen etc.
  }
}
```