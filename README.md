# APX

APX ist eine kleine Sprache, deren Compiler unter Linux direkt ausführbare x86-64-Programme erzeugt.

## Bauen und starten

```sh
make compiler
./apxc beispiel.apx --run
```

Ohne `--run` wird ein ausführbares Programm neben der Quelldatei erzeugt. Mit `-o DATEI` lässt sich der Ausgabepfad festlegen. `./apxc --help` zeigt die Optionen.

## Kleines Beispiel

```apx
zeige "Wie heisst du?"
frage() -> name
zeige "Hallo, " + name

wenn name <> ""
    zeige "Willkommen!"
sonst
    zeige "Kein Name eingegeben."
ende
```

`frage("Prompt: ")` zeigt einen Prompt an und liest eine Zeile. `zahl(text)` wandelt vorzeichenbehaftete 64-Bit-Zahlen um; ungültige und zu große Werte erzeugen einen Laufzeitfehler. `wahr` und `falsch` sind die Werte `1` und `0`.

Auch die englischen Grundwörter bleiben verfügbar: `print`/`zeige`, `if`/`wenn`, `else`/`sonst`, `end`/`ende`, `loop`/`solange`, `repeat`/`wiederhole`, `func`/`funktion`, `give`/`zurueck`, `stop`/`abbruch`, `next`/`weiter` und `undo`/`ruecksetzen`.

## APX-Mechaniken

Jede Zuweisung bewahrt die vorherigen Werte einer Variablen. `wert@1` liest den vorigen Wert; `undo wert` stellt ihn wieder her. `anders(wert)` ist wahr, wenn der aktuelle Wert vom unmittelbar vorherigen abweicht. Für Text wird der Inhalt verglichen.

Listen sind unveränderliche Listen ganzer Zahlen. `attach(liste, wert)` liefert eine neue Liste; frühere Listenfassungen bleiben erhalten:

```apx
liste() -> zahlen
attach(zahlen, 7) -> zahlen
zeige holen(zahlen, 0), len(zahlen)
```

Listenindizes beginnen bei `0`. Ein Zugriff außerhalb der Liste wird als Laufzeitfehler gemeldet.

## Selbsthosting

APX ist noch nicht selbsthostend: Der Compiler ist in C geschrieben. Ein APX-Compiler ist grundsätzlich möglich, aber die Sprache bietet derzeit weder Datei-I/O noch Zugriff auf einzelne Textbytes oder einen veränderbaren Bytepuffer. Diese Fähigkeiten werden für Lexer, Parser und die Ausgabe eines eigenen Compiler-Backends benötigt. Sinnvolle nächste Schritte wären Datei-/Bytefunktionen, dynamische Datenstrukturen und danach ein APX-Compiler, der zunächst C oder einen einfachen Bytecode erzeugt.
