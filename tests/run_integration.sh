#!/usr/bin/env bash
# =============================================================================
# TESTY INTEGRACYJNE / FUNKCJONALNE systemu SIM
#
# Sterują PRAWDZIWYMI binarkami (sim_importer, sim_browser) podając wybory menu
# na stdin (programy czytają je przez std::getline) i weryfikują efekty:
#   - zawartość bazy (przez psql),
#   - strukturę katalogu SIM_ARCHIVE i kopie plików na dysku,
#   - tekst wypisywany przez programy (grep).
#
# Każdy scenariusz startuje od czystej bazy (TRUNCATE). Dane wejściowe to realne
# pliki DICOM z /var/dicom/archive; do scenariuszy sync/import tworzymy izolowaną
# piaskownicę w katalogu tymczasowym, by nie ruszać oryginałów.
#
# Wymagania: skonfigurowane środowisko jak w docs/SETUP_TESTOWY.md
#   (baza sim_medical_db, user sim_user, DSN SIM_DB). init.sql już załadowany.
#
# Użycie:   ./run_integration.sh
# Kod wyjścia: 0 = wszystkie testy PASS, 1 = co najmniej jeden FAIL.
# =============================================================================
set -u

# --- Konfiguracja ---
PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMPORTER="$PROJ_DIR/sim_importer"
BROWSER="$PROJ_DIR/sim_browser"
ARCHIVE_DIR="$PROJ_DIR/SIM_ARCHIVE"   # tworzone przez importDirectory()
DICOM_SRC="/var/dicom/archive"
PSQL=(psql -h localhost -U sim_user -d sim_medical_db -tA)
export PGPASSWORD=sim_password

PASS=0
FAIL=0

# --- Helpery asercji ---
ok()   { PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

assert_eq() { # $1=actual $2=expected $3=name
    if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (oczekiwano [$2], otrzymano [$1])"; fi
}
assert_contains() { # $1=haystack $2=needle $3=name
    if echo "$1" | grep -qF "$2"; then ok "$3"; else bad "$3 (brak [$2] w wyjsciu)"; fi
}
assert_not_contains() { # $1=haystack $2=needle $3=name
    if echo "$1" | grep -qF "$2"; then bad "$3 (znaleziono niechciane [$2])"; else ok "$3"; fi
}

db()       { "${PSQL[@]}" -c "$1"; }
db_scalar(){ "${PSQL[@]}" -c "$1" | head -1; }
reset_db() { db "TRUNCATE studies, patients, data_sources RESTART IDENTITY CASCADE;" >/dev/null; }
clean_archive() { rm -rf "$ARCHIVE_DIR"; }

# Sterowanie binarka: $1=binarka, reszta argumentow to kolejne linie stdin
drive() {
    local bin="$1"; shift
    printf '%s\n' "$@" | (cd "$PROJ_DIR" && "$bin" 2>&1)
}

# --- Sprawdzenia wstepne ---
echo "========== TESTY INTEGRACYJNE SIM =========="
if [ ! -x "$IMPORTER" ] || [ ! -x "$BROWSER" ]; then
    echo "BLAD: brak binarek. Uruchom 'make' w katalogu projektu."; exit 2
fi
if ! db_scalar "SELECT 1;" >/dev/null 2>&1; then
    echo "BLAD: brak polaczenia z baza sim_medical_db. Patrz docs/SETUP_TESTOWY.md."; exit 2
fi
if [ ! -d "$DICOM_SRC" ]; then
    echo "BLAD: brak danych testowych $DICOM_SRC."; exit 2
fi

# =============================================================================
echo
echo "--- Scenariusz 1: Indeksowanie katalogu (bez kopiowania) [5c/5a] ---"
reset_db; clean_archive
OUT=$(drive "$IMPORTER" "1" "$DICOM_SRC" "0")
assert_contains "$OUT" "SUKCES: Zaindeksowano" "indeks: komunikat sukcesu"
P=$(db_scalar "SELECT COUNT(*) FROM patients;")
S=$(db_scalar "SELECT COUNT(*) FROM studies;")
SRC=$(db_scalar "SELECT COUNT(*) FROM data_sources;")
assert_eq "$P" "4"  "indeks: 4 pacjentow (P001-P004)"
assert_eq "$S" "48" "indeks: 48 badan (49 plikow, 1 duplikat UID scalony)"
assert_eq "$SRC" "1" "indeks: 1 zrodlo danych zarejestrowane"
# file_path wskazuje oryginalna lokalizacje (bez kopiowania)
FP=$(db_scalar "SELECT file_path FROM studies LIMIT 1;")
assert_contains "$FP" "/var/dicom/archive" "indeks: file_path = oryginalna sciezka"
# katalog SIM_ARCHIVE NIE powstaje przy indeksowaniu
if [ ! -d "$ARCHIVE_DIR" ]; then ok "indeks: brak kopiowania (SIM_ARCHIVE nie istnieje)"; else bad "indeks: SIM_ARCHIVE nie powinien powstac"; fi

# =============================================================================
echo
echo "--- Scenariusz 2: Idempotencja indeksowania (powtorne uruchomienie) [5b] ---"
OUT=$(drive "$IMPORTER" "1" "$DICOM_SRC" "0")
P2=$(db_scalar "SELECT COUNT(*) FROM patients;")
S2=$(db_scalar "SELECT COUNT(*) FROM studies;")
assert_eq "$P2" "4"  "powtorny indeks: nadal 4 pacjentow (ON CONFLICT DO NOTHING)"
assert_eq "$S2" "48" "powtorny indeks: nadal 48 badan (brak duplikatow)"

# =============================================================================
echo
echo "--- Scenariusz 3: Modalnosci i diakrytyki [5a/5d] ---"
MODS=$(db_scalar "SELECT string_agg(modality||':'||c, ',' ORDER BY modality) FROM (SELECT modality, COUNT(*) c FROM studies GROUP BY modality) t;")
assert_contains "$MODS" "CT:" "modalnosci: obecne CT"
assert_contains "$MODS" "MR:" "modalnosci: obecne MR"
assert_contains "$MODS" "CR:" "modalnosci: obecne CR"
assert_contains "$MODS" "DX:" "modalnosci: obecne DX"
# Diakrytyki: nazwiska Wisniewski/Zielinska zapisane bez polskich znakow
NODIAC=$(db_scalar "SELECT COUNT(*) FROM patients WHERE full_name ~ '[ąćęłńóśźżĄĆĘŁŃÓŚŹŻ]';")
assert_eq "$NODIAC" "0" "diakrytyki: brak polskich znakow w bazie (sanityzacja)"
assert_eq "$(db_scalar "SELECT COUNT(*) FROM patients WHERE full_name LIKE '%Wisniewski%';")" "1" "diakrytyki: Wisniewski (bez 's z kreska')"

# =============================================================================
echo
echo "--- Scenariusz 4: Import z kopiowaniem do archiwum [5b] ---"
reset_db; clean_archive
# Piaskownica: kopiujemy podzbior (P001) do tymczasowego zrodla
SANDBOX=$(mktemp -d)
cp -r "$DICOM_SRC/P001" "$SANDBOX/"
OUT=$(drive "$IMPORTER" "2" "$SANDBOX" "0")
assert_contains "$OUT" "SUKCES: Fizycznie zabezpieczono" "import: komunikat sukcesu"
# Powstal katalog archiwum + kopie plikow
if [ -d "$ARCHIVE_DIR/P001" ]; then ok "import: utworzono SIM_ARCHIVE/P001"; else bad "import: brak SIM_ARCHIVE/P001"; fi
COPIED=$(find "$ARCHIVE_DIR" -name '*.dcm' | wc -l)
DBCOUNT=$(db_scalar "SELECT COUNT(*) FROM studies;")
if [ "$COPIED" -ge 1 ]; then ok "import: skopiowano pliki .dcm ($COPIED)"; else bad "import: nie skopiowano plikow"; fi
assert_eq "$DBCOUNT" "$COPIED" "import: liczba rekordow = liczba skopiowanych plikow"
# file_path wskazuje teraz na archiwum (absolutna sciezka)
FP=$(db_scalar "SELECT file_path FROM studies LIMIT 1;")
assert_contains "$FP" "SIM_ARCHIVE" "import: file_path wskazuje na archiwum"
# struktura: PatientID / Modality_Opis / UID.dcm
if find "$ARCHIVE_DIR/P001" -type d -name 'CT_*' | grep -q .; then ok "import: podfolder Modality_Opis (CT_Glowa)"; else bad "import: brak podfolderu Modality_Opis"; fi

# =============================================================================
echo
echo "--- Scenariusz 5: Idempotencja importu (powtorny import) [5b] ---"
OUT=$(drive "$IMPORTER" "2" "$SANDBOX" "0")
DBCOUNT2=$(db_scalar "SELECT COUNT(*) FROM studies;")
COPIED2=$(find "$ARCHIVE_DIR" -name '*.dcm' | wc -l)
assert_eq "$DBCOUNT2" "$DBCOUNT" "powtorny import: liczba rekordow bez zmian (ON CONFLICT DO UPDATE)"
assert_eq "$COPIED2" "$COPIED"   "powtorny import: liczba plikow bez zmian (overwrite_existing)"

# =============================================================================
echo
echo "--- Scenariusz 6: Synchronizacja - wykrycie usunietego pliku [5c] ---"
# Usuwamy jeden plik z archiwum; sync powinien oznaczyc is_deleted=true (bez kasowania rekordu)
VICTIM=$(find "$ARCHIVE_DIR" -name '*.dcm' | head -1)
VICTIM_UID=$(db_scalar "SELECT study_uid FROM studies WHERE file_path LIKE '%${VICTIM##*/}%' LIMIT 1;")
rm -f "$VICTIM"
DEL_BEFORE=$(db_scalar "SELECT COUNT(*) FROM studies WHERE is_deleted=true;")
OUT=$(drive "$IMPORTER" "3" "0")
assert_contains "$OUT" "oznaczono jako usuniete z dysku: 1" "sync: wykryto 1 usuniety plik"
DEL_AFTER=$(db_scalar "SELECT COUNT(*) FROM studies WHERE is_deleted=true;")
assert_eq "$DEL_AFTER" "$((DEL_BEFORE+1))" "sync: is_deleted ustawione na 1 rekordzie"
# Rekord NADAL istnieje (nie usuniety fizycznie) - zgodnie z wymaganiem 5c
TOTAL_AFTER=$(db_scalar "SELECT COUNT(*) FROM studies;")
assert_eq "$TOTAL_AFTER" "$DBCOUNT" "sync: rekord badania NIE usuniety z bazy (soft-delete)"

# =============================================================================
echo
echo "--- Scenariusz 7: Synchronizacja - auto-rejestracja recznie dodanego pliku [5c] ---"
# Wrzucamy 'recznie' nowy plik DICOM (z P002) do archiwum i synchronizujemy
mkdir -p "$ARCHIVE_DIR/MANUAL"
NEWFILE=$(find "$DICOM_SRC/P002" -name '*.dcm' | head -1)
cp "$NEWFILE" "$ARCHIVE_DIR/MANUAL/recznie_dodany.dcm"
BEFORE=$(db_scalar "SELECT COUNT(*) FROM studies;")
OUT=$(drive "$IMPORTER" "3" "0")
assert_contains "$OUT" "zarejestrowano automatycznie: 1" "sync: auto-rejestracja 1 nowego pliku"
AFTER=$(db_scalar "SELECT COUNT(*) FROM studies;")
assert_eq "$AFTER" "$((BEFORE+1))" "sync: dodano 1 rekord z recznie wrzuconego pliku"

# =============================================================================
echo
echo "--- Scenariusz 8: Synchronizacja jest idempotentna [5c] ---"
OUT=$(drive "$IMPORTER" "3" "0")
assert_contains "$OUT" "zarejestrowano automatycznie: 0" "powtorny sync: 0 nowych (idempotencja)"

# =============================================================================
echo
echo "--- Scenariusz 9: Pliki nie-DICOM / uszkodzone sa ignorowane [niezawodnosc] ---"
reset_db; clean_archive
TRASH=$(mktemp -d)
cp "$DICOM_SRC/P003"/*/*/*.dcm "$TRASH"/ 2>/dev/null
VALID_CNT=$(find "$TRASH" -name '*.dcm' | wc -l)
# dorzucamy smieci: plik tekstowy, pusty plik, plik binarny .dcm bez naglowka
echo "to nie jest dicom" > "$TRASH/notes.txt"
: > "$TRASH/empty.dcm"
head -c 100 /dev/urandom > "$TRASH/garbage.dcm"
OUT=$(drive "$IMPORTER" "1" "$TRASH" "0")
S=$(db_scalar "SELECT COUNT(*) FROM studies;")
# Tylko poprawne DICOM trafiaja do bazy (smieci pominiete)
if [ "$S" -le "$VALID_CNT" ] && [ "$S" -ge 1 ]; then ok "odpornosc: zaindeksowano tylko poprawne DICOM ($S), smieci pominiete"; else bad "odpornosc: nieoczekiwana liczba rekordow ($S, valid=$VALID_CNT)"; fi
rm -rf "$TRASH"

# =============================================================================
echo
echo "--- Scenariusz 10: Wyszukiwanie - po nazwisku, modalnosci, dacie [5d] ---"
reset_db; clean_archive
drive "$IMPORTER" "1" "$DICOM_SRC" "0" >/dev/null
# Po nazwisku
OUT=$(drive "$BROWSER" "2" "Kowalski" "" "" "" "0")
assert_contains "$OUT" "Znaleziono lacznie: 12" "search: Kowalski -> 12 badan"
# Po modalnosci (mala litera -> ILIKE)
OUT=$(drive "$BROWSER" "2" "" "ct" "" "" "0")
assert_contains "$OUT" "Znaleziono lacznie: 12" "search: modalnosc 'ct' -> 12 (case-insensitive)"
# Po zakresie dat (luty-grudzien 2026: P002+P003+P004 = 36)
OUT=$(drive "$BROWSER" "2" "" "" "2026-02-01" "2026-12-31" "0")
assert_contains "$OUT" "Znaleziono lacznie: 36" "search: zakres dat -> 36 badan"
# Brak wynikow
OUT=$(drive "$BROWSER" "2" "NieMaTakiego" "" "" "" "0")
assert_contains "$OUT" "Brak wynikow" "search: nieistniejacy pacjent -> brak wynikow"

# =============================================================================
echo
echo "--- Scenariusz 11: ZABEZPIECZENIE SQL INJECTION (poprawka) [bezpieczenstwo] ---"
# Wejscie "' OR '1'='1" przed poprawka zwracalo WSZYSTKIE rekordy. Po poprawce -> brak.
OUT=$(drive "$BROWSER" "2" "' OR '1'='1" "" "" "" "0")
assert_contains "$OUT" "Brak wynikow" "injection: ' OR '1'='1 -> brak wynikow (apostrofy zescapowane)"
assert_not_contains "$OUT" "SQL Blad" "injection: brak bledu SQL (zapytanie poprawne)"
# Apostrof w nazwisku (np. O'Brien) nie psuje zapytania
OUT=$(drive "$BROWSER" "2" "O'Brien" "" "" "" "0")
assert_not_contains "$OUT" "SQL Blad" "injection: apostrof w nazwisku nie powoduje bledu SQL"
# Srednik / komentarz SQL nie wykonuje dodatkowych polecen
BEFORE=$(db_scalar "SELECT COUNT(*) FROM patients;")
OUT=$(drive "$BROWSER" "2" "x'; DROP TABLE patients; --" "" "" "" "0")
AFTER=$(db_scalar "SELECT COUNT(*) FROM patients;")
assert_eq "$AFTER" "$BEFORE" "injection: proba DROP TABLE nie usunela tabeli patients"

# =============================================================================
echo
echo "--- Scenariusz 12: Statystyki [5d] ---"
OUT=$(drive "$BROWSER" "3" "0")
assert_contains "$OUT" "Liczba zarejestrowanych pacjentow: 4" "stats: 4 pacjentow"
assert_contains "$OUT" "Liczba aktywnych plikow DICOM: 48" "stats: 48 aktywnych plikow"
assert_contains "$OUT" "Wielkosc bazy danych SQL:" "stats: rozmiar bazy"
assert_contains "$OUT" "Podzial wedlug modalnosci" "stats: rozklad modalnosci"

# =============================================================================
echo
echo "--- Scenariusz 13: Ksiega pacjentow [5d] ---"
OUT=$(drive "$BROWSER" "1" "0")
assert_contains "$OUT" "ID: P001" "ksiega: pacjent P001"
assert_contains "$OUT" "ID: P004" "ksiega: pacjent P004"
assert_contains "$OUT" "Liczba zarchiwizowanych plikow:" "ksiega: kolumna z liczba plikow"

# =============================================================================
echo
echo "--- Scenariusz 14: Status [USUNIETY Z ARCHIWUM] w wyszukiwarce [5c/5d] ---"
# Oznaczamy jedno badanie jako usuniete i sprawdzamy, ze wyszukiwarka to pokazuje
db "UPDATE studies SET is_deleted=true WHERE study_uid IN (SELECT study_uid FROM studies LIMIT 1);" >/dev/null
OUT=$(drive "$BROWSER" "2" "" "" "" "" "0")
assert_contains "$OUT" "[USUNIETY Z ARCHIWUM]" "search: status usunietego badania widoczny"
assert_contains "$OUT" "[DOSTEPNY]" "search: status dostepnego badania widoczny"

# =============================================================================
echo
echo "--- Scenariusz 15: Obsluga bledow wejscia [niezawodnosc] ---"
# Nieistniejaca sciezka
OUT=$(drive "$IMPORTER" "1" "/sciezka/ktora/nie/istnieje" "0")
assert_contains "$OUT" "BLAD: Sciezka nie istnieje" "blad: nieistniejaca sciezka obsluzona"
# Nieznana opcja menu
OUT=$(drive "$IMPORTER" "999" "0")
assert_contains "$OUT" "Nieznana opcja" "blad: nieznana opcja menu obsluzona"

# --- Sprzatanie ---
rm -rf "$SANDBOX" 2>/dev/null
clean_archive
reset_db

# =============================================================================
echo
echo "============================================"
echo "PODSUMOWANIE INTEGRACYJNE: $PASS PASS, $FAIL FAIL"
echo "============================================"
[ "$FAIL" -eq 0 ]
