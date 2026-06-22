# Testy systemu SIM

Zestaw testów weryfikujących kompletność i niezawodność projektu 5

## Rodzaje testów

| Plik | Typ | Co weryfikuje |
|---|---|---|
| `unit_test.cpp` | **jednostkowe** | Czyste funkcje silnika: `trim`, `removeDiacritics`, `sanitizeForSQL`, `sanitizeForPath`, `safeDate`, `safeSex` oraz `extractDicomMetadata` (na realnych plikach i na śmieciowych/uszkodzonych). |
| `run_integration.sh` | **integracyjne / funkcjonalne** | Steruje prawdziwymi binarkami `sim_importer` / `sim_browser` (podając wybory menu na stdin) i sprawdza efekty w bazie PostgreSQL oraz na dysku (`SIM_ARCHIVE`). |

## Wymagania wstępne

1. Zbudowane binarki projektu:
   ```bash
   cd .. && make
   ```
2. Skonfigurowane środowisko bazy + DSN ODBC `SIM_DB` - patrz
   [../docs/SETUP_TESTOWY.md](../docs/SETUP_TESTOWY.md). W skrócie: baza `sim_medical_db`,
   user `sim_user`/`sim_password`, schemat z `init.sql`.
3. Dane testowe DICOM w `/var/dicom/archive` (4 pacjentów, 49 plików).
4. `psql` w `PATH` (do asercji na bazie w testach integracyjnych).

## Uruchomienie

```bash
# tylko testy jednostkowe
make unit

# tylko testy integracyjne/funkcjonalne
make integration        # albo: ./run_integration.sh

# wszystko
make test

# sprzątanie binarki testowej
make clean
```

Kod wyjścia `0` = wszystkie testy PASS; `1` = co najmniej jeden FAIL (przydatne w CI).

## Co pokrywają testy integracyjne (15 scenariuszy)

Każdy scenariusz oznaczony jest wymaganiem PDF, które weryfikuje:

- **Sc. 1–2** - indeksowanie katalogu bez kopiowania + idempotencja (5a, 5c).
- **Sc. 3** - różne modalności (CT/MR/CR/DX) i poprawna sanityzacja diakrytyków (5a).
- **Sc. 4–5** - import z kopiowaniem do `SIM_ARCHIVE`, struktura katalogów
  `Pacjent/Modalność_Opis/UID.dcm`, idempotencja powtórnego importu (5b).
- **Sc. 6** - wykrycie usuniętego pliku → `is_deleted=true` bez kasowania rekordu (5c).
- **Sc. 7–8** - auto-rejestracja ręcznie dorzuconego pliku + idempotencja sync (5c).
- **Sc. 9** - odporność: pliki nie-DICOM / puste / uszkodzone są ignorowane.
- **Sc. 10** - wyszukiwanie po nazwisku, modalności (case-insensitive), zakresie dat,
  brak wyników (5d).
- **Sc. 11** - **zabezpieczenie SQL injection** (poprawka): `' OR '1'='1`, apostrof
  w nazwisku, próba `DROP TABLE` - żadne nie psuje bazy ani zapytania.
- **Sc. 12** - statystyki (liczba pacjentów/plików, rozmiar bazy, rozkład modalności) (5d).
- **Sc. 13** - księga pacjentów (5d).
- **Sc. 14** - status `[USUNIĘTY Z ARCHIWUM]` / `[DOSTĘPNY]` w wynikach (5c/5d).
- **Sc. 15** - obsługa błędów wejścia (nieistniejąca ścieżka, nieznana opcja menu).

## Uwagi

- Testy integracyjne modyfikują bazę `sim_medical_db` (TRUNCATE przed scenariuszami)
  oraz tworzą/usuwają katalog `SIM_ARCHIVE` w katalogu projektu. Na końcu sprzątają po sobie.
- Liczba 48 badań przy 49 plikach jest poprawna - jeden `SOPInstanceUID` się powtarza
  i jest deduplikowany przez `ON CONFLICT` (zob. dokumentacja, sekcja *Znane ograniczenia*).
- Testy jednostkowe dołączają `db_handler.cpp` przez `#include`, więc nie wymagają zmian
  w kodzie produkcyjnym; funkcje ODBC nie są w nich wywoływane (potrzebne tylko do linkowania).
