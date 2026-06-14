# INSTRUKCJA INSTALACJI SYSTEMU ARCHIWIZACJI SIM (PACS)

Ten dokument przeprowadzi Cię krok po kroku przez proces instalacji bazy danych i uruchomienia projektu w środowisku Linux/WSL.

---

## KROK 1: INSTALACJA WYMAGANYCH PROGRAMÓW

Otwórz terminal (konsolę) i wpisz poniższe komendy, aby zainstalować bazę danych PostgreSQL oraz sterowniki ODBC. Zatwierdź każdą z nich klawiszem `Enter`:

```bash
sudo apt update
sudo apt install postgresql postgresql-contrib unixodbc unixodbc-dev odbc-postgresql libdcmtk-dev zlib1g-dev
```

---

## KROK 2: STWORZENIE BAZY DANYCH

Zanim system zacznie działać, musimy stworzyć dla niego bazę danych i wgrać schemat.

**1. Zaloguj się jako administrator bazy:**
Wpisz w terminalu komendę:
```bash
sudo -u postgres psql
```

**2. Stwórz użytkownika i bazę:**
Twój znak zachęty w terminalu zmieni się na `postgres=#`. Wklej tam poniższe trzy linijki (każdą zatwierdź Enterem), aby utworzyć środowisko i z niego wyjść:
```sql
CREATE USER sim_user WITH PASSWORD 'sim_password';
CREATE DATABASE sim_medical_db OWNER sim_user;
\q
```

**3. Wgraj strukturę tabel:**
Upewnij się, że plik `init.sql` znajduje się w tym samym folderze, w którym aktualnie jesteś w terminalu. Następnie wpisz:
```bash
sudo -u postgres psql -d sim_medical_db -f init.sql
```

---

## KROK 3: KONFIGURACJA POŁĄCZENIA ODBC

Nasz program w C++ musi wiedzieć, gdzie szukać bazy danych. Zrobimy to edytując globalny plik konfiguracyjny systemu Linux.

**1. Otwórz plik konfiguracyjny w edytorze tekstowym:**
Wpisz w terminalu:
```bash
sudo nano /etc/odbc.ini
```

**2. Wklej konfigurację:**
Skopiuj poniższy blok tekstu i wklej go w całości do otwartego edytora:
```ini
[SIM_DB]
Description = Baza danych dla projektu SIM
Driver      = PostgreSQL Unicode
Servername  = localhost
Port        = 5432
Database    = sim_medical_db
UserName    = sim_user
Password    = sim_password
```
> **Ważne:** Zapisz plik wciskając na klawiaturze `Ctrl+O`, potem `Enter`, a na koniec `Ctrl+X` by wyjść z edytora.

**3. Przetestuj połączenie:**
Wpisz w terminalu komendę testową:
```bash
isql -v SIM_DB sim_user sim_password
```
*Jeśli zobaczysz napis `Connected!`, wpisz `quit` aby zamknąć test. Baza jest w pełni gotowa do pracy z programem!*

---

## KROK 4: KOMPILACJA I URUCHOMIENIE PROGRAMU

Wróć do folderu z projektem (tam, gdzie znajdują się pliki `.cpp` oraz plik `Makefile`).

**1. Skompiluj kod:**
Aby oczyścić system ze starych plików i zbudować program od nowa, wpisz w terminalu:
```bash
make clean
make
```

**2. Uruchom system:**
Otrzymasz dwa gotowe programy wykonywalne. Uruchamiasz je wpisując w terminalu odpowiednią komendę:

Aby włączyć moduł importu i synchronizacji (Zarządca), wpisz:
```bash
./sim_importer
```

Aby włączyć wyszukiwarkę i księgę pacjentów (Lekarz), wpisz:
```bash
./sim_browser
```