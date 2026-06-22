# Dokumentacja techniczna systemu SIM - Baza medycznych obrazowych badań diagnostycznych

> **Projekt 5** z dokumentu `SIM_Projekty_2026L.pdf`:
> *„Baza medycznych obrazowych badań diagnostycznych”*.
> Politechnika Warszawska, WEiTI, Inżynieria Biomedyczna - Systemy Informatyczne w Medycynie.

Dokument opisuje całość rozwiązania: architekturę, schemat bazy, wykorzystane biblioteki
i wzorce, działanie każdej funkcji (z odsyłaczami do linii kodu) oraz krok po kroku
przebieg każdej operacji w module Zarządcy i module Lekarza. Na końcu znajduje się sekcja
*Znane ograniczenia i ryzyka* z opisem przypadków brzegowych.

---

## Spis treści
1. [Cel projektu](#1-cel-projektu)
2. [Architektura systemu](#2-architektura-systemu)
3. [Schemat bazy danych](#3-schemat-bazy-danych)
4. [Wykorzystane technologie i biblioteki](#4-wykorzystane-technologie-i-biblioteki)
5. [Wspólny silnik DICOM - `dicom_handler.cpp`](#5-wspólny-silnik-dicom--dicom_handlercpp)
6. [Wspólny silnik bazodanowy - `db_handler.cpp`](#6-wspólny-silnik-bazodanowy--db_handlercpp)
7. [Moduł Zarządcy (`sim_importer`) - krok po kroku](#7-moduł-zarządcy-sim_importer--krok-po-kroku)
8. [Moduł Lekarza (`sim_browser`) - krok po kroku](#8-moduł-lekarza-sim_browser--krok-po-kroku)
9. [Mechanizmy bezpieczeństwa i czyszczenia danych](#9-mechanizmy-bezpieczeństwa-i-czyszczenia-danych)
10. [Znane ograniczenia i ryzyka](#10-znane-ograniczenia-i-ryzyka)
11. [Kompilacja i uruchomienie](#11-kompilacja-i-uruchomienie)

---

## 1. Cel projektu

Celem jest utworzenie rozwiązania pozwalającego gromadzić metadane obrazowych badań medycznych różnych modalności
(CR, CT, MR, DX, US, NM, PT, …) w relacyjnej bazie danych PostgreSQL, przy założeniu, że
same pliki obrazowe (DICOM) pozostają na dysku, a do bazy trafiają wyłącznie metadane
oraz informacja, gdzie fizycznie znajduje się plik (archiwum / źródło danych).

| Wymaganie projektowe | Realizacja w kodzie |
|---|---|
| **5a. Przechowywanie badań** - baza trzyma badania w wielu katalogach dyskowych ujętych w tabeli źródeł danych; w tabelach zapisane są dane identyfikacyjne pacjentów i metadane badań. | Tabela `data_sources` (katalogi-źródła), `patients` (dane pacjenta), `studies` (metadane + `file_path`, `file_size`). Patrz [init.sql](../init.sql). |
| **5b. Import badań** - kopiowanie z zadanego katalogu na katalog bazy, aktualizacja list pacjentów/badań, obsługa powtórnego przenoszenia. | `importDirectory()` - kopiuje pliki do `./SIM_ARCHIVE`, wstawia/aktualizuje rekordy; powtórny import obsłużony przez `INSERT … ON CONFLICT (study_uid) DO UPDATE`. [db_handler.cpp:199](../db_handler.cpp#L199) |
| **5c. Aktualizacja indeksu** - skanowanie katalogów źródeł; usunięcie pliku → odnotowane w tabelach (bez kasowania pacjenta/badania); ręcznie skopiowane pliki → dodanie do tabel. | `indexDirectory()` (skan bez kopiowania) [db_handler.cpp:126](../db_handler.cpp#L126) oraz `syncDatabase()` - krok 1: oznacza `is_deleted=true` dla brakujących plików; krok 2: auto-rejestruje pliki dorzucone ręcznie. [db_handler.cpp:298](../db_handler.cpp#L298) |
| **5d. Wyszukiwanie i filtrowanie** - po dacie, typie badania, danych pacjenta itp. | `searchPatient()` [db_handler.cpp:398](../db_handler.cpp#L398) |
| Komunikacja z bazą przez **ODBC**. | unixODBC (`<sql.h>`, `<sqlext.h>`), połączenie przez DSN `SIM_DB`. [db_handler.cpp:108](../db_handler.cpp#L108) |
| Oddzielny program do wyszukiwania i prostej analizy statystycznej. | `sim_browser` + `showStatistics()`. [db_handler.cpp:490](../db_handler.cpp#L490) |

---

## 2. Architektura systemu

System składa się z **dwóch programów wykonywalnych** zbudowanych na **wspólnym silniku**:

```
                 ┌───────────────────────────────────────────────┐
                 │            WSPÓLNY SILNIK (.o)                │
                 │                                               │
   DICOM (DCMTK) │  dicom_handler.cpp  → extractDicomMetadata()  │
                 │  db_handler.cpp     → cała logika + SQL/ODBC  │
                 └───────────────┬───────────────┬───────────────┘
                                 │               │
              ┌──────────────────┘               └──────────────────┐
              │                                                     │
   ┌──────────▼───────────┐                          ┌──────────────▼──────────┐
   │  sim_importer        │                          │  sim_browser            │
   │  (main_import.cpp)   │                          │  (main_search.cpp)      │
   │  MODUŁ ZARZĄDCY      │                          │  MODUŁ LEKARZA          │
   │  1. Indeksuj         │                          │  1. Księga pacjentów    │
   │  2. Importuj         │                          │  2. Wyszukiwanie        │
   │  3. Synchronizuj     │                          │  3. Statystyki          │
   └──────────┬───────────┘                          └──────────────┬──────────┘
              │                  ODBC (DSN: SIM_DB)                 │
              └──────────────────────┬──────────────────────────────┘
                                     │
                          ┌──────────▼───────────┐        ┌─────────────────────┐
                          │  PostgreSQL          │        │  Dysk:              │
                          │  sim_medical_db      │        │  ./SIM_ARCHIVE/...  │
                          │  data_sources        │◄──────►│  /var/dicom/...     │
                          │  patients, studies   │ metad. │  (pliki .dcm)       │
                          └──────────────────────┘        └─────────────────────┘
```

Podział na pliki:

| Plik | Rola |
|---|---|
| [main_import.cpp](../main_import.cpp) | Pętla menu Zarządcy (`std::getline` → wybór 1/2/3/0). |
| [main_search.cpp](../main_search.cpp) | Pętla menu Lekarza (`std::getline` → wybór 1/2/3/0). |
| [dicom_handler.h](../dicom_handler.h) / [.cpp](../dicom_handler.cpp) | Struktura `DicomMetadata` + funkcja `extractDicomMetadata()`. |
| [db_handler.h](../db_handler.h) / [.cpp](../db_handler.cpp) | Deklaracje 8 operacji + cała implementacja (ODBC, SQL, sanityzacja, I/O plików). |
| [init.sql](../init.sql) | Tworzenie schematu i nadanie uprawnień. |
| [Makefile](../Makefile) | Buduje obie binarki ze wspólnych `.o`. |

Zarządca (administrator PACS) ma uprawnienia modyfikujące archiwum (import, sync), 
Lekarz dostaje narzędzie wyłącznie do odczytu/wyszukiwania. 
Obie binarki łączą się z tą samą bazą tym samym DSN.

---

## 3. Schemat bazy danych

Zdefiniowany w [init.sql](../init.sql). Trzy tabele odpowiadające modelowi **źródło → pacjent → badanie**:

### `data_sources` - tabela źródeł danych (wymaganie 5a)
| Kolumna | Typ | Znaczenie |
|---|---|---|
| `id` | `SERIAL PRIMARY KEY` | Identyfikator źródła |
| `directory_path` | `VARCHAR(512) NOT NULL` | Ścieżka katalogu-źródła lub archiwum |
| `last_scanned` | `TIMESTAMP` | Czas ostatniego skanowania (`NOW()` przy wstawianiu) |

### `patients` - dane identyfikacyjne pacjenta
| Kolumna | Typ | Znaczenie |
|---|---|---|
| `patient_id` | `VARCHAR(100) PRIMARY KEY` | DICOM `PatientID` (np. `P001`) |
| `full_name` | `VARCHAR(255)` | DICOM `PatientName` (format `Nazwisko^Imię`) |
| `birth_date` | `DATE` | DICOM `PatientBirthDate` lub `NULL` |
| `sex` | `VARCHAR(1)` | `M`/`F`/`O` lub `NULL` |

### `studies` - metadane badań/obrazów
| Kolumna | Typ | Znaczenie |
|---|---|---|
| `study_uid` | `VARCHAR(128) PRIMARY KEY` | DICOM `SOPInstanceUID` (klucz deduplikacji) |
| `patient_id` | `VARCHAR(100) → patients` | Klucz obcy do pacjenta |
| `source_id` | `INT → data_sources` | Klucz obcy do źródła |
| `modality` | `VARCHAR(10)` | Mdalność badania (CT, MR, CR, DX, …) |
| `study_date` | `DATE` | DICOM `StudyDate` lub `NULL` |
| `study_desc` | `TEXT` | Opis badania (Study/Series/Protocol) |
| `file_path` | `VARCHAR(512)` | Fizyczna lokalizacja pliku na dysku |
| `file_size` | `BIGINT` | Rozmiar pliku w bajtach |
| `is_deleted` | `BOOLEAN DEFAULT false` | Znacznik **„plik zniknął z dysku”** (wymaganie 5c) |

> **Uwaga projektowa:** kluczem badania jest `SOPInstanceUID` (identyfikator pojedynczego
> obrazu/instancji), a nie `StudyInstanceUID`. W praktyce oznacza to, że jeden rekord
> `studies` = jeden plik DICOM. To upraszcza model do dwóch poziomów (pacjent → plik),
> kosztem braku jawnego poziomu „badanie”/„seria” (patrz [ograniczenia](#10-znane-ograniczenia-i-ryzyka)).

---

## 4. Wykorzystane technologie i biblioteki

| Technologia | Zastosowanie | Gdzie |
|---|---|---|
| **C++17** | `std::filesystem`, structured bindings, `std::string`. | flaga `-std=c++17` w [Makefile](../Makefile) |
| **DCMTK** (`dcmdata`, `oflog`, `ofstd`) | Parsowanie plików DICOM, odczyt tagów. | `dctk.h`, `findAndGetString`, [dicom_handler.cpp](../dicom_handler.cpp) |
| **unixODBC** (`-lodbc`) | Komunikacja z bazą wg standardu **ODBC** (wymóg PDF). | `<sql.h>`, `<sqlext.h>`, [db_handler.cpp](../db_handler.cpp) |
| **PostgreSQL** | Silnik bazy danych (`sim_medical_db`). | DSN `SIM_DB` |
| **zlib** (`-lz`) | Wymagana przez DCMTK (kompresja). | [Makefile](../Makefile) |
| **`std::filesystem`** | Rekurencyjne skanowanie, kopiowanie plików, tworzenie katalogów. | `recursive_directory_iterator`, `copy_file`, `create_directories` |

Wzorce/rozwiązania:
- **ODBC handle lifecycle**: `SQLAllocHandle` (ENV→DBC→STMT) → `SQLConnect` → operacje →
  `SQLFreeHandle`/`SQLFreeStmt`. Patrz `connectToDatabase()` [db_handler.cpp:108](../db_handler.cpp#L108).
- **Upsert (idempotencja importu)**: `INSERT … ON CONFLICT … DO UPDATE/DO NOTHING` -
  pozwala bezpiecznie powtarzać import (wymaganie 5b „powtórne przenoszenie”).
- **Soft-delete**: kolumna `is_deleted` zamiast fizycznego usuwania rekordu (wymaganie 5c
  „nie należy usuwać informacji o pacjencie i badaniu”).
- **Bezpieczne wczytywanie wejścia**: wszędzie `std::getline` zamiast `std::cin >>`, co
  eliminuje typowe problemy z pozostawionym znakiem nowej linii w buforze.

---

## 5. Wspólny silnik DICOM - `dicom_handler.cpp`

### `struct DicomMetadata` ([dicom_handler.h:8](../dicom_handler.h#L8))
Kontener na wyciągnięte metadane: `patientId`, `patientName`, `birthDate`, `patientSex`,
`sopUid`, `modality`, `studyDate`, `studyDesc`, `fileSize`, `filePath`.

### `bool extractDicomMetadata(const std::string& filePath, DicomMetadata& data)` ([dicom_handler.cpp:9](../dicom_handler.cpp#L9))
Jedyna funkcja silnika DICOM. Działanie krok po kroku:

1. **Wczytanie pliku**: `DcmFileFormat::loadFile()`. Jeśli `OFCondition` nie jest `good()`
   → `return false` (plik nie jest DICOM / uszkodzony). [dicom_handler.cpp:11](../dicom_handler.cpp#L11)
2. **Pobranie zbioru danych**: `getDataset()`.
3. **Odczyt tagów** przez `findAndGetString` z każdorazową walidacją wskaźnika (`&& p`):
   - `DCM_PatientID`, `DCM_PatientName`, `DCM_Modality`, `DCM_StudyDate`,
     `DCM_PatientBirthDate`, `DCM_PatientSex`, `DCM_SOPInstanceUID`.
   - **Imię pacjenta**: jeśli puste → `"BRAK DANYCH"` (anonimizacja/brak). [dicom_handler.cpp:31](../dicom_handler.cpp#L31)
   - **Opis badania**: kaskada `StudyDescription` → `SeriesDescription` → `ProtocolName`
     → `"BRAK_OPISU"`. Zapewnia sensowny opis nawet gdy główny tag jest pusty. [dicom_handler.cpp:50](../dicom_handler.cpp#L50)
4. **Walidacja minimalna**: jeśli `patientId` **lub** `sopUid` jest puste → `return false`.
   To jest definicja „poprawnego DICOM” w tym systemie. [dicom_handler.cpp:64](../dicom_handler.cpp#L64)
5. **Rozmiar pliku**: `std::filesystem::file_size()`; zapis `filePath`. `return true`.

> **Konsekwencja**: pliki nie-DICOM, uszkodzone albo DICOM bez `PatientID`/`SOPInstanceUID`
> są po cichu pomijane przez wszystkie operacje (indeksowanie, import, sync). To celowe
> i pożądane - chroni bazę przed śmieciowymi wpisami.

---

## 6. Wspólny silnik bazodanowy - `db_handler.cpp`

### Funkcje pomocnicze (czyszczenie / bezpieczeństwo)

| Funkcja | Linia | Działanie |
|---|---|---|
| `printOdbcError()` | [12](../db_handler.cpp#L12) | Wypisuje komunikat błędu SQL z `SQLGetDiagRec`. |
| `trim()` | [20](../db_handler.cpp#L20) | Usuwa białe znaki z początku/końca napisu. |
| `replaceAll()` | [30](../db_handler.cpp#L30) | Zamienia wszystkie wystąpienia podłańcucha. |
| `removeDiacritics()` | [40](../db_handler.cpp#L40) | Zamienia znaki diakrytyczne (ą→a, ż→z, é→e, …) na ASCII. |
| `sanitizeForPath()` | [53](../db_handler.cpp#L53) | Buduje bezpieczną nazwę katalogu/pliku: zostawia `[A-Za-z0-9_-]`, spacja/`^`→`_`. |
| `sanitizeForSQL()` | [69](../db_handler.cpp#L69) | Usuwa znaki sterujące/nie-ASCII, podwaja apostrofy (`'`→`''`), `trim`. |
| `safeDate()` | [85](../db_handler.cpp#L85) | Zwraca formę `'YYYYMMDD'` albo `NULL`, jeśli data < 8 znaków/pusta. |
| `safeSex()` | [97](../db_handler.cpp#L97) | Zwraca `'M'`/`'F'`/`'O'` (pierwsza litera, wielka) albo `NULL`. |

### Zarządzanie połączeniem
- `connectToDatabase()` [108](../db_handler.cpp#L108): alokuje uchwyty ENV/DBC, ustawia
  `SQL_OV_ODBC3`, łączy się przez `SQLConnect(... "SIM_DB", "sim_user", "sim_password" ...)`.
  Zwraca `nullptr` przy błędzie (program `main` przerywa się z kodem 1).
- `disconnectDatabase()` [121](../db_handler.cpp#L121): `SQLDisconnect` + `SQLFreeHandle`.

> **Uwaga**: dane logowania (DSN, user, hasło) są zaszyte w kodzie. To upraszcza
> uruchomienie, ale stanowi ograniczenie (patrz sekcja 10).

Pozostałe funkcje (`indexDirectory`, `importDirectory`, `syncDatabase`, `searchPatient`,
`showStatistics`, `listAllPatients`) opisane są w sekcjach 7–8 jako kroki operacji.

---

## 7. Moduł Zarządcy (`sim_importer`) - krok po kroku

Pętla menu: [main_import.cpp](../main_import.cpp). Po starcie łączy się z bazą; przy
braku połączenia kończy z kodem 1. Menu czyta wybór przez `std::getline`.

### Operacja 1 - „Indeksuj katalog dyskowy (bez kopiowania)” → `indexDirectory()` [db_handler.cpp:126](../db_handler.cpp#L126)

Realizuje aktualizację indeksu dla zewnętrznego źródła danych (pliki pozostają na miejsc).

1. **Pobranie ścieżki**: `getline` + `trim`. Jeśli `!fs::exists` → błąd i powrót.
2. **Rejestracja źródła**: `INSERT INTO data_sources(directory_path, last_scanned) … RETURNING id;`
   Pobranie nowego `source_id` przez `SQLBindCol` + `SQLFetch`. [db_handler.cpp:136](../db_handler.cpp#L136)
3. **Budowa listy plików**:
   - pojedynczy plik → lista 1-elementowa,
   - katalog → `recursive_directory_iterator` z `skip_permission_denied` (pomija zablokowane foldery). [db_handler.cpp:151](../db_handler.cpp#L151)
4. **Dla każdego pliku**:
   - `extractDicomMetadata()`; jeśli `false` → plik pominięty.
   - Sanityzacja pól: `sanitizeForSQL`, `safeDate`, `safeSex`.
   - `INSERT INTO patients … ON CONFLICT DO NOTHING;` (pacjent dodany raz). [db_handler.cpp:180](../db_handler.cpp#L180)
   - `INSERT INTO studies … ON CONFLICT (study_uid) DO UPDATE SET is_deleted=false, file_path=…, source_id=…, study_desc=…;`
   - przy ponownym indeksowaniu rekord jest „ożywiany” i aktualizowany. [db_handler.cpp:186](../db_handler.cpp#L186)
5. **Podsumowanie**: liczba zaindeksowanych plików.

> `file_path` w tym trybie wskazuje oryginalną lokalizację pliku (nie kopiuje go).

### Operacja 2 - „Importuj nowe badania (kopiuj do Archiwum)” → `importDirectory()` [db_handler.cpp:199](../db_handler.cpp#L199)

Realizuje wymaganie 5b: kopiowanie do katalogu zarządzanego `./SIM_ARCHIVE`.

1. Pobranie ścieżki źródłowej (+ `trim`, sprawdzenie istnienia).
2. Utworzenie katalogu archiwum `./SIM_ARCHIVE` (jeśli nie istnieje).
3. Rejestracja archiwum jako źródła (`RETURNING id`).
4. Budowa listy plików (jak wyżej).
5. Dla każdego poprawnego DICOM budowana jest hierarchia katalogów archiwum:
   ```
   SIM_ARCHIVE/<sanitizeForPath(PatientID)>/<Modality>_<Opis>/<sanitizeForPath(SOPInstanceUID)>.dcm
   ```
   - folder pacjenta (po `PatientID`, fallback `UNKNOWN_PATIENT`), [db_handler.cpp:250](../db_handler.cpp#L250)
   - podfolder badania (`Modality_Opis`), [db_handler.cpp:256](../db_handler.cpp#L256)
   - docelowa ścieżka pliku (`absoluteDest`). [db_handler.cpp:264](../db_handler.cpp#L264)
6. `INSERT patients … ON CONFLICT DO NOTHING` oraz
   `INSERT studies … ON CONFLICT (study_uid) DO UPDATE …` z bezwzględną ścieżką `absoluteDest`.
7. Dopiero po udanym zapisie do bazy kopiuje plik:
   `fs::copy_file(originalPath, destPath, overwrite_existing)`. [db_handler.cpp:285](../db_handler.cpp#L285)
   - kolejność „najpierw baza, potem kopia” gwarantuje, że nie zostanie skopiowany plik,
     którego nie udało się zaindeksować.
   - `overwrite_existing` → powtórny import jest idempotentny (nadpisuje kopię, aktualizuje rekord).
8. Podsumowanie: liczba zabezpieczonych plików.

### Operacja 3 - „Synchronizuj bazę (sprawdź spójność)” → `syncDatabase()` [db_handler.cpp:298](../db_handler.cpp#L298)

Realizuje wymaganie 5c - dwukierunkowa synchronizacja.

**Krok 1/2 - z bazy na dysk (wykrywanie usuniętych):**
1. `SELECT study_uid, file_path FROM studies WHERE is_deleted=false;`
2. Dla każdego rekordu sprawdza `fs::exists` i `fs::is_regular_file` (po `trim` ścieżki).
3. Brakujące pliki → lista `missingUids`.
4. Dla każdego: `UPDATE studies SET is_deleted=true WHERE study_uid=…;`
   - rekord pozostaje w bazie, jednakże oznaczony jako usunięty (zgodnie z PDF). [db_handler.cpp:324](../db_handler.cpp#L324)

**Krok 2/2 - z dysku do bazy (wykrywanie ręcznie dorzuconych):**
1. Skan `./SIM_ARCHIVE` (`recursive_directory_iterator`, `skip_permission_denied`).
2. Dla każdego pliku: `extractDicomMetadata` (nie-DICOM ignorowane).
3. `SELECT COUNT(*) FROM studies WHERE study_uid=…;` - jeśli `0`, plik jest nowy.
4. Wstawienie pacjenta i badania (`ON CONFLICT DO NOTHING`, `source_id`=1). [db_handler.cpp:378](../db_handler.cpp#L378)
5. Podsumowanie obu kroków.

> Efekt: usunięcie pliku z dysku oznaczy go jako `is_deleted`, a ręczne wrzucenie nowego
> pliku do archiwum zarejestruje go automatycznie przy następnej synchronizacji.

---

## 8. Moduł Lekarza (`sim_browser`) - krok po kroku

Pętla menu: [main_search.cpp](../main_search.cpp). Tylko odczyt danych.

### Operacja 1 - „Wyświetl księgę pacjentów” → `listAllPatients()` [db_handler.cpp:525](../db_handler.cpp#L525)
1. `SELECT p.patient_id, p.full_name, COUNT(s.study_uid) FROM patients p
   LEFT JOIN studies s ON p.patient_id=s.patient_id GROUP BY … ORDER BY p.full_name LIMIT 100;`
   - `LEFT JOIN` → pacjent bez badań też się pokaże (liczba = 0).
2. Wypisuje `ID | Pacjent | Liczba plików`. Puste nazwisko → `[ZANONIMIZOWANO]`. [db_handler.cpp:545](../db_handler.cpp#L545)
3. Gdy brak pacjentów → komunikat „Brak pacjentów w bazie.”

### Operacja 2 - „Zaawansowane wyszukiwanie badań” → `searchPatient()` [db_handler.cpp:398](../db_handler.cpp#L398)
1. Pobranie 4 kryteriów (`getline` + `trim`): fraza (imię/nazwisko/ID), modalność, data OD, data DO.
   Każde pole może być puste.
2. Sanityzacja wejścia (poprawka bezpieczeństwa): `sanitizeForSQL` dla frazy i modalności,
   `safeDate` dla dat. [db_handler.cpp:421](../db_handler.cpp#L421)
3. Budowa filtrów - sklejane warunki `AND`:
   - fraza → `p.full_name ILIKE '%…%' OR p.patient_id ILIKE '%…%'` (case-insensitive),
   - modalność → `s.modality ILIKE '%…%'`,
   - daty → `s.study_date >= …` / `<= …` (tylko jeśli przeszły walidację `safeDate`).
4. Zliczenie: `SELECT COUNT(*) … WHERE 1=1 <filtry>;` → `totalFound`.
   Jeśli `0` → „Brak wyników” i powrót. [db_handler.cpp:433](../db_handler.cpp#L433)
5. Pobranie danych: ta sama klauzula + `ORDER BY s.study_date DESC LIMIT 100;`.
   Wypisuje `Pacjent | Modalność | Opis | Data | [DOSTĘPNY]/[USUNIĘTY Z ARCHIWUM]`. [db_handler.cpp:454](../db_handler.cpp#L454)
   - status czytany z `is_deleted` z obsługą reprezentacji `'t'`/`'1'`/`'T'`. [db_handler.cpp:473](../db_handler.cpp#L473)
6. Podsumowanie: łączna liczba; jeśli > 100 - informacja, że pokazano 100 najnowszych (paginacja wyników)

> Wzorzec **count + limit** oddziela „ile jest wszystkich” od „pokaż pierwsze 100”, dzięki
> czemu interfejs nie zalewa lekarza tysiącami wierszy, a wciąż podaje pełną liczbę trafień.

### Operacja 3 - „Statystyki systemu” → `showStatistics()` [db_handler.cpp:490](../db_handler.cpp#L490)
Cztery zapytania (prosta analiza statystyczna, wymaganie 5d / „prosta analiza”):
1. `SELECT COUNT(*) FROM patients;` - liczba pacjentów.
2. `SELECT COUNT(*) FROM studies WHERE is_deleted=false;` - liczba aktywnych plików.
3. `SELECT pg_size_pretty(pg_database_size('sim_medical_db'));` - rozmiar bazy na dysku.
4. `SELECT modality, COUNT(*) … GROUP BY modality ORDER BY COUNT(*) DESC;` - rozkład modalności.

---

## 9. Mechanizmy bezpieczeństwa i czyszczenia danych

- **Sanityzacja ścieżek** (`sanitizeForPath`): chroni system plików przed niebezpiecznymi
  nazwami (znaki specjalne, diakrytyki) przy budowie struktury `SIM_ARCHIVE`.
- **Sanityzacja SQL** (`sanitizeForSQL`): podwaja apostrofy i odrzuca znaki nie-ASCII -
  stosowana konsekwentnie zarówno przy zapisie (import/index/sync), jak i przy odczycie (wyszukiwarka). Patrz [sekcja 10, pkt 1](#10-znane-ograniczenia-i-ryzyka).
- **Walidacja typów** (`safeDate`, `safeSex`): zamienia uszkodzone/niekompletne dane DICOM
  na `NULL`, zamiast wstawiać do bazy wartości łamiące typ kolumny.
- **Soft-delete** (`is_deleted`): zachowuje historię (wymóg PDF: „nie usuwać informacji”).
- **Kolejność operacji w imporcie**: zapis do bazy → dopiero kopia pliku.

---

## 10. Znane ograniczenia i ryzyka

1. **SQL injection w wyszukiwarce** - *naprawione*.
   Wcześniej pola `phrase`/`modality`/`dateFrom`/`dateTo` w `searchPatient()` trafiały do
   zapytania bez unikania apostrofów, podczas gdy `sanitizeForSQL` było używane tylko
   przy imporcie. Wejście `' OR '1'='1` zwracało wszystkie rekordy (potwierdzone:
   48 badań „wyciekało” mimo braku uprawnień do takiego zapytania).
   *Poprawka*: wartości tekstowe przepuszczone przez `sanitizeForSQL`, daty przez `safeDate`
   ([db_handler.cpp:421](../db_handler.cpp#L421)). Po poprawce ten sam input zwraca 0 wyników
   Przetestowane: `test_search_injection`.
   > Docelowo właściwym rozwiązaniem byłoby użycie zapytań parametryzowanych ODBC
   > (`SQLBindParameter`), co eliminuje problem u źródła; sanityzacja jest dobrym
   > zabezpieczeniem warstwowym, ale parametryzacja jest standardem branżowym.

2. **`std::stoi` na nietypowych danych** - w `searchPatient` (count) i `syncDatabase`
   (`COUNT(*)`) wynik konwertowany jest `std::stoi((char*)countStr)`. Dla poprawnego
   `SELECT COUNT(*)` jest to bezpieczne (zawsze liczba), ale brak odpowiedniej obsługi wyjątkow oznacza, że nieoczekiwany pusty wynik rzuciłby wyjątek. Niskie ryzyko przy obecnych zapytaniach.

3. **Klucz badania = `SOPInstanceUID`** - model jest faktycznie dwupoziomowy (pacjent → plik).
   Nie ma osobnych encji *Study* i *Series*. Konsekwencja: jeden „rekord badania” = jeden
   obraz; brak grupowania obrazów w serie/badania. Dla wymagań zadania 5 wystarczające,
   ale projekty 6 i 7 wymagają już modelu czteropoziomowego.

4. **Kolizje `sanitizeForPath`** - usuwanie znaków specjalnych może teoretycznie zmapować
   dwa różne `SOPInstanceUID` na tę samą nazwę pliku. W praktyce UID-y DICOM to cyfry i
   kropki, więc po sanityzacji pozostają unikalne; ryzyko realne tylko dla nietypowych UID.

5. **Zaszyte poświadczenia** - DSN/user/hasło są w kodzie ([db_handler.cpp:114](../db_handler.cpp#L114)).
   Brak konfigurowalności (np. zmiennych środowiskowych). Akceptowalne dla projektu
   dydaktycznego; w produkcji wymagałoby odseparowania konfiguracji.

6. **`source_id` w `syncDatabase`** - krok 2 wpisuje `source_id`=1 na sztywno
   ([db_handler.cpp:378](../db_handler.cpp#L378)), zakładając, że pierwsze źródło to archiwum.
   Działa, dopóki rekord o `id=1` istnieje; przy nietypowej kolejności tworzenia źródeł
   mogłoby wskazywać nie to źródło.

7. **Brak transakcji** - operacje import/index wykonują wiele `INSERT`/`UPDATE` poza
   jawną transakcją; przerwanie w połowie zostawia bazę częściowo zaktualizowaną
   (przy ponownym uruchomieniu `ON CONFLICT` i tak doprowadzi do spójności).

8. **Deduplikacja po UID** - pliki o identycznym `SOPInstanceUID` są scalane do jednego
   rekordu (`ON CONFLICT`). W zbiorze testowym `/var/dicom/archive` to celowo występuje:
   **49 plików → 48 rekordów** `studies` (jeden UID się powtarza). To poprawne zachowanie
   PACS (ten sam obraz nie powinien być zdublowany), udokumentowane testem integracyjnym.

---

## 11. Kompilacja i uruchomienie

```bash
make clean && make          # buduje sim_importer i sim_browser
./sim_importer              # Moduł Zarządcy (import/indeks/sync)
./sim_browser               # Moduł Lekarza (przeglądanie/wyszukiwanie/statystyki)
```

Wymagania środowiska (baza, DSN ODBC) - patrz [SETUP_TESTOWY.md](SETUP_TESTOWY.md) oraz
oryginalny [README.md](../README.md). Testy automatyczne - patrz [../tests/README.md](../tests/README.md).
