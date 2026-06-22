// =============================================================================
// db_handler.cpp - SILNIK BAZODANOWY I CAŁA LOGIKA OPERACJI SIM
//
// Połączenie ODBC,funkcje czyszczące/walidujące dane oraz 
// implementacje 6 operacji wywoływanych z menu obu binarek 
// (indexDirectory, importDirectory, syncDatabase oraz
// searchPatient, showStatistics, listAllPatients).
//
// Konwencja budowania SQL: zapytania składane są jako napisy, a każda wartość
// pochodząca z plików/użytkownika MUSI przejść przez sanitizeForSQL / safeDate /
// safeSex zanim trafi do zapytania. To warstwowa ochrona przed SQL injection
// i przed danymi łamiącymi typ kolumny (np. uszkodzona data DICOM).
// =============================================================================

#include "db_handler.h"
#include "dicom_handler.h"
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// Wypisuje na stderr czytelny komunikat ostatniego błędu SQL dla danego uchwytu
// (statement/connection). SQLGetDiagRec zwraca standardowy 5-znakowy SQLSTATE
// oraz tekst błędu sterownika. Używane po nieudanych SQLExecDirect, żeby zamiast
// cichej porażki administrator zobaczył, co poszło nie tak.
void printOdbcError(SQLHANDLE handle, SQLSMALLINT type) {
    SQLCHAR sqlState[6], errorMsg[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER nativeError; SQLSMALLINT msgLen;
    if (SQLGetDiagRec(type, handle, 1, sqlState, &nativeError, errorMsg, sizeof(errorMsg), &msgLen) == SQL_SUCCESS) {
        std::cerr << "   [Baza Danych SQL Blad]: " << errorMsg << " (Stan: " << sqlState << ")\n";
    }
}

// Usuwa białe znaki (spacje, tabulatory, znaki nowej linii) z POCZĄTKU i KOŃCA
// napisu, modyfikując go w miejscu. Potrzebne, bo ścieżki/kryteria wpisane przez
// użytkownika często mają zbędną spację lub znak '\r' (zwł. przy danych z Windows).
void trim(std::string& s) {
    if (s.empty()) return;
    std::size_t first = s.find_first_not_of(" \n\r\t");
    if (first == std::string::npos) { s.clear(); return; } // sam biały znak -> pusty napis
    std::size_t last = s.find_last_not_of(" \n\r\t");
    s = s.substr(first, last - first + 1);
}

// --- KROK 1: NARZĘDZIA DO CZYSZCZENIA DANYCH ---

// Zamienia WSZYSTKIE wystąpienia podłańcucha 'from' na 'to' (w miejscu).
// Po każdej podmianie przeskakujemy o długość 'to' (a nie 'from'), dzięki czemu
// nie wpadamy w nieskończoną pętlę, gdy 'to' zawiera 'from'. Pusty 'from'
// byłby pętlą bez końca - stąd wczesny return.
void replaceAll(std::string& str, const std::string& from, const std::string& to) {
    if(from.empty()) return;
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

// Zamienia polskie (i kilka zachodnioeuropejskich) znaków diakrytycznych na ich
// odpowiedniki ASCII (ż->z, ł->l, ó->o ...). Po co? Bo nazwiska/opisy z DICOM
// bywają w UTF-8, a chcemy: (1) bezpieczne, jednobajtowe nazwy katalogów archiwum,
// (2) spójne wyszukiwanie ("Wisniewski" znajdzie "Wiśniewski"). Operuje na bajtach
// UTF-8 (każdy polski znak to 2 bajty), dlatego podmieniamy całe sekwencje.
std::string removeDiacritics(std::string s) {
    replaceAll(s, "ą", "a"); replaceAll(s, "ć", "c"); replaceAll(s, "ę", "e");
    replaceAll(s, "ł", "l"); replaceAll(s, "ń", "n"); replaceAll(s, "ó", "o");
    replaceAll(s, "ś", "s"); replaceAll(s, "ź", "z"); replaceAll(s, "ż", "z");
    replaceAll(s, "é", "e"); replaceAll(s, "ü", "u"); replaceAll(s, "ö", "o");
    
    replaceAll(s, "Ą", "A"); replaceAll(s, "Ć", "C"); replaceAll(s, "Ę", "E");
    replaceAll(s, "Ł", "L"); replaceAll(s, "Ń", "N"); replaceAll(s, "Ó", "O");
    replaceAll(s, "Ś", "S"); replaceAll(s, "Ź", "Z"); replaceAll(s, "Ż", "Z");
    replaceAll(s, "É", "E"); replaceAll(s, "Ü", "U"); replaceAll(s, "Ö", "O");
    return s;
}

// Buduje bezpieczną nazwę dla katalogu/pliku z dowolnego napisu (PatientID, opis,
// modalność, UID). Zostawia tylko znaki bez ryzyka dla systemu plików:
// litery/cyfry oraz '-' i '_'. Spację i '^' (separator w PatientName "Nazwisko^Imię")
// zamienia na '_', a wszystko inne (/, \, :, *, ?, kropki) po prostu usuwa.
// Cel: żaden plik DICOM nie utworzy ścieżki wychodzącej poza archiwum ani
// nazwy nielegalnej na danym systemie plików.
std::string sanitizeForPath(std::string input) {
    input = removeDiacritics(input);
    std::string output;
    for (char c : input) {
        unsigned char uc = static_cast<unsigned char>(c);
        // isalnum na unsigned char - bezpieczne dla bajtów >127 (uniknięcie UB).
        if (isalnum(uc) || c == '-' || c == '_') {
            output += c;
        } else if (c == ' ' || c == '^') {
            output += '_';
        }
        // pozostałe znaki celowo pomijamy
    }
    return output;
}

// Przygotowuje napis do wstawienia wewnątrz definicji SQL ('...'). Dwa zadania:
//   1) usuwa znaki sterujące i nie-ASCII (uc<32 lub uc>126) - chroni przed
//      "dziwnymi" bajtami z plików i ujednolica dane,
//   2) podwaja apostrof (' -> ''), bo to jedyny znak, który mógłby zamknąć
//      definicję i wstrzyknąć kod SQL. To kluczowy element ochrony przed SQL injection.
// Wynik wstawia się następnie między ręcznie dopisane apostrofy w zapytaniu.
std::string sanitizeForSQL(const std::string& input) {
    std::string normalized = removeDiacritics(input);
    std::string output;
    for (char c : normalized) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc > 126) continue;     // odrzuć znaki sterujące / nie-ASCII
        else if (c == '\'') output += "''";     // anty-injection
        else output += c;
    }
    trim(output);
    return output;
}

// =========================================================================
// BEZPIECZNA DATA - zwraca gotowy fragment SQL: albo format 'YYYYMMDD',
// albo słowo kluczowe NULL (bez apostrofów). Dzięki temu wywołujący może
// wkleić wynik wprost do zapytania, niezależnie od tego, czy data była poprawna.
// Poprawna data DICOM (typ DA) ma 8 znaków; krótsza/pusta/uszkodzona -> NULL,
// żeby nie wstawiać do kolumny DATE wartości, która wywaliłaby zapytanie.
// =========================================================================
std::string safeDate(std::string dateStr) {
    dateStr = sanitizeForSQL(dateStr); // najpierw escape + odrzucenie śmieci
    if (dateStr.empty() || dateStr.length() < 8) {
        return "NULL";
    }
    return "'" + dateStr + "'";
}

// =========================================================================
// BEZPIECZNA PŁEĆ - DICOM dopuszcza M/F/O, ale w danych trafiają się śmieci
// ("Unknown", puste, całe słowa). Bierzemy tylko PIERWSZĄ literę, podnosimy do
// wielkiej i akceptujemy wyłącznie M/F/O. Cokolwiek innego -> NULL. Zwraca gotowy
// fragment SQL ('M' / 'F' / 'O' / NULL), więc pasuje wprost do kolumny VARCHAR(1).
// =========================================================================
std::string safeSex(std::string sexStr) {
    sexStr = sanitizeForSQL(sexStr);
    if (sexStr.empty()) return "NULL";

    char c = toupper(sexStr[0]); // pierwsza litera, wielka
    if (c == 'M' || c == 'F' || c == 'O') {
        return std::string("'") + c + "'";
    }
    return "NULL"; // np. "Unknown" / "X" -> bezpieczny NULL zamiast błędu
}

// Nawiązuje połączenie z bazą przez ODBC. Sekwencja alokacji uchwytów jest
// standardowa dla unixODBC: ENV (środowisko) -> ustawienie wersji ODBC3 ->
// DBC (połączenie). SQLConnect używa nazwanego źródła danych (DSN "SIM_DB"),
// dzięki czemu adres serwera/baza są w konfiguracji ODBC, a nie w kodzie.
// Zwraca uchwyt połączenia lub nullptr przy niepowodzeniu (wywołujący kończy program).
SQLHDBC connectToDatabase() {
    SQLHENV hEnv; SQLHDBC hDbc;
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

    // Uwaga: DSN, użytkownik i hasło są zaszyte w kodzie (ograniczenie projektu
    // DSN "SIM_DB" musi istnieć w konfiguracji ODBC.
    if (!SQL_SUCCEEDED(SQLConnect(hDbc, (SQLCHAR*)"SIM_DB", SQL_NTS, (SQLCHAR*)"sim_user", SQL_NTS, (SQLCHAR*)"sim_password", SQL_NTS))) {
        std::cerr << "BLAD KRYTYCZNY: Brak polaczenia z baza przez ODBC!\n";
        return nullptr;
    }
    return hDbc;
}

// Zamyka połączenie i zwalnia uchwyt DBC. Wywoływane raz, przy wyjściu z programu.
void disconnectDatabase(SQLHDBC hDbc) {
    SQLDisconnect(hDbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
}

// =========================================================================
// OPERACJA 1 (Zarządca): INDEKSOWANIE - wymaganie 5c/5a.
// Skanuje wskazany katalog/plik i zapisuje do bazy metadane znalezionych badań
// DICOM, ale NIE kopiuje plików - file_path wskazuje oryginalną lokalizację.
// Stosowane do zewnętrznych źródeł danych (dysk, nośnik), które mają zostać
// w miejscu, a jedynie być widoczne w bazie.
// =========================================================================
void indexDirectory(SQLHDBC hDbc) {
    // 1. Pobranie i oczyszczenie ścieżki podanej przez użytkownika.
    std::string targetDir;
    std::cout << "\nPodaj sciezke do katalogu lub pliku do ZAINDEKSOWANIA: ";
    std::getline(std::cin, targetDir);
    trim(targetDir);

    // Wczesne odrzucenie nieistniejącej ścieżki - bez sensu cokolwiek skanować.
    if (!fs::exists(targetDir)) { std::cerr << "BLAD: Sciezka nie istnieje!\n"; return; }

    // Uchwyt instrukcji (statement) - używany do wszystkich zapytań tej operacji.
    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // 2. Rejestracja źródła danych. RETURN id zwraca świeżo nadane id (SERIAL),
    //    którym potem oznaczymy wszystkie badania z tego skanowania (source_id).
    std::string sourceQuery = "INSERT INTO data_sources (directory_path, last_scanned) VALUES ('" + sanitizeForSQL(targetDir) + "', NOW()) RETURNING id;";
    if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)sourceQuery.c_str(), SQL_NTS))) {
        printOdbcError(hStmt, SQL_HANDLE_STMT);
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }
    // Odczyt zwróconego id: dowiązujemy kolumnę 1 do zmiennej sourceId, pobieramy wiersz,
    // zamykamy kursor. Wartość początkowa 1 to fallback, gdyby RETURN nic nie dał.
    long sourceId = 1; SQLLEN cbSourceId;
    SQLBindCol(hStmt, 1, SQL_C_LONG, &sourceId, 0, &cbSourceId); SQLFetch(hStmt); SQLFreeStmt(hStmt, SQL_CLOSE);

    int filesProcessed = 0;
    std::cout << "Analiza zrodla i przygotowanie do indeksowania...\n";

    // 3. Zbudowanie listy plików do przetworzenia. Obsługujemy 2 przypadki, bo
    //    użytkownik może podać pojedynczy plik albo cały katalog.
    std::vector<std::string> filesToProcess;

    if (fs::is_regular_file(targetDir)) {
        filesToProcess.push_back(targetDir);  // pojedynczy plik
    } else if (fs::is_directory(targetDir)) {
        // Rekurencyjne przejście po katalogu. skip_permission_denied sprawia, że
        // zablokowane podkatalogi systemowe są pomijane, a nie wywracają iteracji.
        for (const auto& entry : fs::recursive_directory_iterator(targetDir, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                filesToProcess.push_back(entry.path().string());
            }
        }
    } else {
        // np. urządzenie blokowe / FIFO - ani plik, ani katalog.
        std::cerr << "BLAD: Sciezka nie jest prawidlowym plikiem ani katalogiem!\n";
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }

    std::cout << "Rozpoczynam indeksowanie " << filesToProcess.size() << " plikow...\n";

    // 4. Przetworzenie każdego pliku z listy.
    for (const auto& pathStr : filesToProcess) {
        DicomMetadata data;
        // Pliki nie-DICOM / uszkodzone zwrócą false i zostaną cicho pominięte.
        if (extractDicomMetadata(pathStr, data)) {

            // Wszystkie pola tekstowe przepuszczamy przez sanitizeForSQL,
            // a daty/płeć przez safeDate/safeSex (zwracają gotowe literały lub NULL).
            std::string s_id = sanitizeForSQL(data.patientId);
            std::string s_name = sanitizeForSQL(data.patientName);
            std::string s_uid = sanitizeForSQL(data.sopUid);
            std::string s_desc = sanitizeForSQL(data.studyDesc);

            std::string s_bDate = safeDate(data.birthDate);
            std::string s_sDate = safeDate(data.studyDate);
            std::string s_sex = safeSex(data.patientSex);

            // Wstawienie pacjenta. ON CONFLICT DO NOTHING: jeśli pacjent o tym
            // patient_id już istnieje, nic nie zmieniamy (kolejne pliki tego samego
            // pacjenta nie nadpisują jego danych).
            std::string insertPatient = "INSERT INTO patients (patient_id, full_name, birth_date, sex) VALUES ('" + s_id + "', '" + s_name + "', " + s_bDate + ", " + s_sex + ") ON CONFLICT DO NOTHING;";

            if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertPatient.c_str(), SQL_NTS))) {
                printOdbcError(hStmt, SQL_HANDLE_STMT);
            }

            // Wstawienie badania. ON CONFLICT (study_uid) DO UPDATE: jeśli badanie o
            // tym SOPInstanceUID już jest, "ożywiamy" je (is_deleted=false) i
            // aktualizujemy ścieżkę/źródło/opis. To realizuje indempotencję i
            // ponowne indeksowanie pliku, który wcześniej zniknął i wrócił. EXCLUDED
            // to wartości, które próbowaliśmy wstawić.
            std::string insertStudy = "INSERT INTO studies (study_uid, patient_id, source_id, modality, study_date, study_desc, file_path, file_size) VALUES ('" + s_uid + "', '" + s_id + "', " + std::to_string(sourceId) + ", '" + sanitizeForSQL(data.modality) + "', " + s_sDate + ", '" + s_desc + "', '" + sanitizeForSQL(data.filePath) + "', " + std::to_string(data.fileSize) + ") ON CONFLICT (study_uid) DO UPDATE SET is_deleted = false, file_path = EXCLUDED.file_path, source_id = EXCLUDED.source_id, study_desc = EXCLUDED.study_desc;";

            if (SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertStudy.c_str(), SQL_NTS))) {
                filesProcessed++;
            } else {
                printOdbcError(hStmt, SQL_HANDLE_STMT);
            }
        }
    }
    std::cout << "SUKCES: Zaindeksowano plikow w bazie: " << filesProcessed << "\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt); // zwolnienie uchwytu statement
}

// =========================================================================
// OPERACJA 2 (Zarządca): IMPORT Z KOPIOWANIEM - wymaganie 5b.
// Podobnie jak indeksowanie, ale dodatkowo kopiuje każdy poprawny plik DICOM
// do zarządzanego archiwum ./SIM_ARCHIVE, układając go w czytelną hierarchię
// katalogów. W bazie zapisuje się ścieżka do kopii w archiwum (bezwzględna).
// Powtórny import jest bezpieczny (idempotentny) dzięki ON CONFLICT + overwrite.
// =========================================================================
void importDirectory(SQLHDBC hDbc) {
    // 1. Ścieżka źródłowa (np. pendrive, katalog z badaniami).
    std::string targetDir;
    std::cout << "\nPodaj zrodlo danych do IMPORTU (np. zewnetrzny dysk/pendrive, lub pojedynczy plik): ";
    std::getline(std::cin, targetDir);
    trim(targetDir);

    if (!fs::exists(targetDir)) { std::cerr << "BLAD: Sciezka nie istnieje!\n"; return; }

    // 2. Katalog zarządzany przez bazę (archiwum). Tworzymy go, jeśli nie istnieje.
    std::string managedStorage = "./SIM_ARCHIVE";
    if (!fs::exists(managedStorage)) fs::create_directories(managedStorage);

    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // 3. Jako źródło rejestrujemy archiwum (a nie katalog wejściowy) - bo to tam
    //    fizycznie wylądują pliki i tam baza będzie ich szukać przy synchronizacji.
    std::string sourceQuery = "INSERT INTO data_sources (directory_path, last_scanned) VALUES ('" + sanitizeForSQL(managedStorage) + "', NOW()) RETURNING id;";
    if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)sourceQuery.c_str(), SQL_NTS))) {
        printOdbcError(hStmt, SQL_HANDLE_STMT);
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }
    long sourceId = 1; SQLLEN cbSourceId;
    SQLBindCol(hStmt, 1, SQL_C_LONG, &sourceId, 0, &cbSourceId); SQLFetch(hStmt); SQLFreeStmt(hStmt, SQL_CLOSE);

    int filesProcessed = 0;
    std::cout << "Analiza zrodla i przygotowanie do operacji...\n";

    // 4. Lista plików - identycznie jak w indexDirectory (plik lub cały katalog).
    std::vector<std::string> filesToProcess;

    if (fs::is_regular_file(targetDir)) {
        filesToProcess.push_back(targetDir); // pojedynczy plik
    } else if (fs::is_directory(targetDir)) {
        for (const auto& entry : fs::recursive_directory_iterator(targetDir, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                filesToProcess.push_back(entry.path().string());
            }
        }
    } else {
        std::cerr << "BLAD: Sciezka nie jest prawidlowym plikiem ani katalogiem!\n";
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }

    std::cout << "Rozpoczynam przetwarzanie " << filesToProcess.size() << " plikow...\n";

    // 5. Główna pętla: dla każdego poprawnego DICOM budujemy ścieżkę docelową
    //    w archiwum, zapisujemy metadane, a na końcu kopiujemy plik.
    for (const auto& originalPath : filesToProcess) {
        DicomMetadata data;

        if (extractDicomMetadata(originalPath, data)) {

            // 5a. FOLDER PACJENTA - segregacja po PatientID. Pusty/dziwny ID -> fallback.
            std::string safePatientId = sanitizeForPath(data.patientId);
            if (safePatientId.empty()) safePatientId = "UNKNOWN_PATIENT";

            std::string patientFolder = managedStorage + "/" + safePatientId;
            if (!fs::exists(patientFolder)) fs::create_directories(patientFolder);

            // 5b. PODFOLDER BADANIA - "Modalnosc_Opis" (np. CT_Glowa) czyni archiwum
            //     czytelnym dla człowieka przeglądającego dysk. Puste wartości -> domyślne.
            std::string safeModality = sanitizeForPath(data.modality.empty() ? "UNKNOWN" : data.modality);
            std::string safeDesc = sanitizeForPath(data.studyDesc.empty() ? "BRAK_OPISU" : data.studyDesc);

            std::string studySubfolder = patientFolder + "/" + safeModality + "_" + safeDesc;
            if (!fs::exists(studySubfolder)) fs::create_directories(studySubfolder);

            // 5c. NAZWA PLIKU = oczyszczony SOPInstanceUID + ".dcm" (gwarancja unikalności).
            //     Zapisujemy ścieżkę bezwzględną, bo synchronizacja sprawdza istnienie
            //     pliku niezależnie od bieżącego katalogu roboczego.
            std::string destPath = studySubfolder + "/" + sanitizeForPath(data.sopUid) + ".dcm";
            std::string absoluteDest = fs::absolute(destPath).string();

            std::string s_id = sanitizeForSQL(data.patientId);
            std::string s_name = sanitizeForSQL(data.patientName);
            std::string s_uid = sanitizeForSQL(data.sopUid);
            std::string s_desc = sanitizeForSQL(data.studyDesc);
            
            std::string s_bDate = safeDate(data.birthDate);
            std::string s_sDate = safeDate(data.studyDate);
            std::string s_sex = safeSex(data.patientSex);

            std::string insertPatient = "INSERT INTO patients (patient_id, full_name, birth_date, sex) VALUES ('" + s_id + "', '" + s_name + "', " + s_bDate + ", " + s_sex + ") ON CONFLICT DO NOTHING;";
            
            if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertPatient.c_str(), SQL_NTS))) {
                printOdbcError(hStmt, SQL_HANDLE_STMT);
            }
            
            // Zapis metadanych z file_path wskazującym na kopię w archiwum (absoluteDest).
            // ON CONFLICT (study_uid) DO UPDATE → powtórny import nadpisuje rekord zamiast
            // tworzyć duplikat (idempotencja, wymaganie 5b "powtórne przenoszenie").
            std::string insertStudy = "INSERT INTO studies (study_uid, patient_id, source_id, modality, study_date, study_desc, file_path, file_size) VALUES ('" + s_uid + "', '" + s_id + "', " + std::to_string(sourceId) + ", '" + sanitizeForSQL(data.modality) + "', " + s_sDate + ", '" + s_desc + "', '" + sanitizeForSQL(absoluteDest) + "', " + std::to_string(data.fileSize) + ") ON CONFLICT (study_uid) DO UPDATE SET is_deleted = false, file_path = EXCLUDED.file_path, source_id = EXCLUDED.source_id, study_desc = EXCLUDED.study_desc;";

            // KOLEJNOŚĆ MA ZNACZENIE: najpierw zapis do bazy, a fizyczną kopię pliku
            // robimy dopiero po udanym INSERT. Dzięki temu nie zostanie skopiowany plik,
            // którego nie udało się zarejestrować. overwrite_existing → powtórny import
            // nadpisuje wcześniejszą kopię zamiast się wywalić.
            if (SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertStudy.c_str(), SQL_NTS))) {
                fs::copy_file(originalPath, destPath, fs::copy_options::overwrite_existing);
                filesProcessed++;
            } else {
                printOdbcError(hStmt, SQL_HANDLE_STMT);
            }
        }
    }
    std::cout << "SUKCES: Fizycznie zabezpieczono i zapisano w bazie plikow: " << filesProcessed << "\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}
// =========================================================================
// OPERACJA 3 (Zarządca): DWUKIERUNKOWA SYNCHRONIZACJA - wymaganie 5c.
// Uzgadnia stan bazy ze stanem dysku w obie strony:
//   KROK 1 (baza -> dysk): badania, których plik zniknął, oznaczamy is_deleted=true
//                          (rekord nie jest kasowany - historia zostaje).
//   KROK 2 (dysk -> baza): pliki DICOM dorzucone ręcznie do archiwum, których nie ma
//                          w bazie, rejestrujemy automatycznie.
// Operacja jest idempotentna - kolejne uruchomienie bez zmian na dysku nic nie zmienia.
// =========================================================================
void syncDatabase(SQLHDBC hDbc) {
    std::cout << "\n--- URUCHAMIANIE DWUKIERUNKOWEJ SYNCHRONIZACJI SYSTEMU ---\n";
    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // ============ KROK 1: BAZA -> DYSK (wykrywanie usuniętych plików) ============
    // Pobieramy wszystkie AKTYWNE badania i sprawdzamy, czy ich pliki nadal są na dysku.
    std::string selectQuery = "SELECT study_uid, file_path FROM studies WHERE is_deleted = false;";
    SQLExecDirect(hStmt, (SQLCHAR*)selectQuery.c_str(), SQL_NTS);

    SQLCHAR uid[128], path[512]; SQLLEN cbUid, cbPath;
    std::vector<std::string> missingUids;

    // UWAGA: nie aktualizujemy bazy w trakcie tej pętli, bo kursor SELECT jest otwarty.
    // Najpierw zbieramy listę brakujących UID-ów, a UPDATE-y robimy po zamknięciu kursora.
    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLGetData(hStmt, 1, SQL_C_CHAR, uid, sizeof(uid), &cbUid);
        SQLGetData(hStmt, 2, SQL_C_CHAR, path, sizeof(path), &cbPath);

        std::string checkPath = (char*)path;
        trim(checkPath); // ścieżka z bazy bywa dopełniona spacjami przez sterownik

        // Plik zniknął (skasowany / odmontowany dysk / ścieżka sieciowa). is_regular_file
        // dodatkowo odrzuca przypadek, gdy ścieżka wskazuje teraz na katalog.
        if (!fs::exists(checkPath) || !fs::is_regular_file(checkPath)) {
            missingUids.push_back((char*)uid);
        }
    }
    SQLFreeStmt(hStmt, SQL_CLOSE); // zamykamy kursor SELECT przed UPDATE-ami

    // Oznaczenie brakujących jako usunięte. SOFT-DELETE: rekord zostaje, zmienia się
    // tylko flaga (wymóg PDF "nie usuwać informacji o pacjencie i badaniu").
    int markedDeleted = 0;
    for (const auto& mUid : missingUids) {
        std::string updateQuery = "UPDATE studies SET is_deleted = true WHERE study_uid = '" + mUid + "';";
        SQLExecDirect(hStmt, (SQLCHAR*)updateQuery.c_str(), SQL_NTS);
        markedDeleted++;
    }
    std::cout << "[Krok 1/2] Wykryto i oznaczono jako usuniete z dysku: " << markedDeleted << " badan.\n";

    // ============ KROK 2: DYSK -> BAZA (auto-rejestracja ręcznie dodanych) ============
    std::string managedStorage = "./SIM_ARCHIVE";
    int autoRegistered = 0;

    if (fs::exists(managedStorage)) {
        std::cout << "[Krok 2/2] Skanowanie Archiwum w poszukiwaniu recznie dodanych plikow...\n";

        for (const auto& entry : fs::recursive_directory_iterator(managedStorage, fs::directory_options::skip_permission_denied)) {

            // Sprawdzamy każdy zwykły plik, niezależnie od rozszerzenia - to
            // extractDicomMetadata zdecyduje, czy to DICOM. Pliki innych formatów
            // (NIfTI, JPEG itp.) zwrócą false i zostaną pominięte.
            if (entry.is_regular_file()) {
                std::string currentPath = fs::absolute(entry.path()).string();

                DicomMetadata data;
                if (extractDicomMetadata(currentPath, data)) {
                    std::string s_uid = sanitizeForSQL(data.sopUid);

                    // Czy to badanie już jest w bazie? Liczymy wystąpienia po study_uid.
                    std::string checkQuery = "SELECT COUNT(*) FROM studies WHERE study_uid = '" + s_uid + "';";
                    SQLExecDirect(hStmt, (SQLCHAR*)checkQuery.c_str(), SQL_NTS);

                    SQLCHAR countStr[20]; SQLLEN cbCount;
                    int exists = 0;
                    if (SQLFetch(hStmt) == SQL_SUCCESS) {
                        SQLGetData(hStmt, 1, SQL_C_CHAR, countStr, sizeof(countStr), &cbCount);
                        exists = std::stoi((char*)countStr); // COUNT(*) zawsze zwróci liczbę
                    }
                    SQLFreeStmt(hStmt, SQL_CLOSE);

                    // exists==0 → plik leży w archiwum, ale baza o nim nie wie → rejestrujemy.
                    if (exists == 0) {
                        std::string s_id = sanitizeForSQL(data.patientId);
                        std::string s_name = sanitizeForSQL(data.patientName);
                        std::string s_desc = sanitizeForSQL(data.studyDesc);

                        std::string s_bDate = safeDate(data.birthDate);
                        std::string s_sDate = safeDate(data.studyDate);
                        std::string s_sex = safeSex(data.patientSex);

                        // Pacjent (idempotentnie). safeDate/safeSex same wstawiają apostrofy lub NULL.
                        std::string insPat = "INSERT INTO patients (patient_id, full_name, birth_date, sex) VALUES ('" + s_id + "', '" + s_name + "', " + s_bDate + ", " + s_sex + ") ON CONFLICT DO NOTHING;";
                        SQLExecDirect(hStmt, (SQLCHAR*)insPat.c_str(), SQL_NTS);

                        // Badanie. source_id=1 (zakładamy, że id=1 to archiwum - patrz
                        // "Znane ograniczenia" w dokumentacji). file_path = realna lokalizacja pliku.
                        std::string insStd = "INSERT INTO studies (study_uid, patient_id, source_id, modality, study_date, study_desc, file_path, file_size) VALUES ('" + s_uid + "', '" + s_id + "', 1, '" + sanitizeForSQL(data.modality) + "', " + s_sDate + ", '" + s_desc + "', '" + sanitizeForSQL(currentPath) + "', " + std::to_string(data.fileSize) + ") ON CONFLICT DO NOTHING;";

                        if (SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insStd.c_str(), SQL_NTS))) {
                            autoRegistered++;
                        }
                    }
                }
            }
        }
    }
    std::cout << "[Krok 2/2] Pomyslnie wykryto i zarejestrowano automatycznie: " << autoRegistered << " nowych plikow.\n";
    std::cout << "--- SYNCHRONIZACJA ZAKONCZONA SUKCESEM ---\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}



// =========================================================================
// OPERACJA 2 (Lekarz): ZAAWANSOWANE WYSZUKIWANIE - wymaganie 5d.
// Pozwala filtrować badania po fragmencie nazwiska/ID pacjenta, modalności oraz
// zakresie dat. Każde pole jest opcjonalne (puste = brak filtra). Realizacja
// dwustopniowa: najpierw COUNT (ile pasuje łącznie), potem właściwe dane z LIMIT 100.
// =========================================================================
void searchPatient(SQLHDBC hDbc) {
    std::string phrase, modality, dateFrom, dateTo;

    std::cout << "\n--- ZAAWANSOWANE WYSZUKIWANIE BADAN ---\n";
    std::cout << "(Mozesz zostawic dowolne pole puste i wcisnac Enter)\n\n";

    // Pobranie 4 kryteriów. Każde może zostać puste - wtedy nie tworzy filtra.
    std::cout << "1. Fragment imienia, nazwiska lub ID pacjenta: ";
    std::getline(std::cin, phrase);

    std::cout << "2. Typ badania (mozna mala litera, np. ct, mr, us): ";
    std::getline(std::cin, modality);

    std::cout << "3. Data badania OD (Format YYYY-MM-DD lub puste): ";
    std::getline(std::cin, dateFrom);

    std::cout << "4. Data badania DO (Format YYYY-MM-DD lub puste): ";
    std::getline(std::cin, dateTo);

    trim(phrase);
    trim(modality);
    trim(dateFrom);
    trim(dateTo);

    // --- ZABEZPIECZENIE PRZED SQL INJECTION ---
    // Kryteria pochodzą od użytkownika i są wklejane wprost do zapytania, więc
    // każde z nich musi zostać oczyszczone, tak samo jak dane przy imporcie:
    //  - tekst (fraza, modalność) -> sanitizeForSQL (escapowanie apostrofów),
    //  - daty -> safeDate (zwraca gotowy literał 'YYYY-MM-DD' albo słowo NULL).
    // Bez tego wejście typu  ' OR '1'='1  zwróciłoby WSZYSTKIE rekordy.
    std::string s_phrase   = sanitizeForSQL(phrase);
    std::string s_modality = sanitizeForSQL(modality);
    std::string s_dateFrom = safeDate(dateFrom); // "'YYYY-MM-DD'" lub "NULL"
    std::string s_dateTo   = safeDate(dateTo);

    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // 1. BUDOWANIE WSPÓLNYCH FILTRÓW. Składamy je raz i używamy w obu zapytaniach
    //    (COUNT i SELECT), żeby liczba i wyświetlone wiersze pochodziły z tego samego
    //    warunku. "1=1" na początku WHERE pozwala doklejać kolejne "AND ..." bez
    //    sprawdzania, czy to pierwszy warunek.
    std::string filters = "";
    // ILIKE = porównanie bez rozróżniania wielkości liter; '%...%' = dowolny fragment.
    if (!s_phrase.empty())   filters += " AND (p.full_name ILIKE '%" + s_phrase + "%' OR p.patient_id ILIKE '%" + s_phrase + "%')";
    if (!s_modality.empty()) filters += " AND s.modality ILIKE '%" + s_modality + "%'";
    // safeDate zwróciło "NULL", jeśli data była pusta/niepoprawna - wtedy pomijamy filtr.
    if (s_dateFrom != "NULL") filters += " AND s.study_date >= " + s_dateFrom;
    if (s_dateTo != "NULL")   filters += " AND s.study_date <= " + s_dateTo;

    // ==========================================================
    // KROK 1: Zliczenie wszystkich pasujących wyników (bez limitu).
    // Dzięki temu możemy pokazać prawdziwą łączną liczbę trafień, nawet jeśli
    // za chwilę wyświetlimy tylko 100 najnowszych.
    // ==========================================================
    std::string countQuery = "SELECT COUNT(*) FROM patients p JOIN studies s ON p.patient_id = s.patient_id WHERE 1=1" + filters + ";";
    SQLExecDirect(hStmt, (SQLCHAR*)countQuery.c_str(), SQL_NTS);

    SQLCHAR totalStr[20]; SQLLEN cbTotal;
    int totalFound = 0;
    if (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLGetData(hStmt, 1, SQL_C_CHAR, totalStr, sizeof(totalStr), &cbTotal);
        totalFound = std::stoi((char*)totalStr); // COUNT(*) zawsze zwraca liczbę
    }
    SQLFreeStmt(hStmt, SQL_CLOSE); // zamknięcie kursora COUNT przed kolejnym zapytaniem

    // Brak trafień - kończymy od razu, nie ma sensu uruchamiać drugiego zapytania.
    if (totalFound == 0) {
        std::cout << "\nBrak wynikow dla podanych kryteriow.\n---------------------------\n";
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }

    // ==========================================================
    // KROK 2: POBRANIE WŁAŚCIWYCH DANYCH. ORDER BY study_date DESC = najnowsze
    // na górze; LIMIT 100 chroni przed zalaniem ekranu przy dużych zbiorach (paginacja).
    // ==========================================================
    std::string query = "SELECT p.full_name, s.study_desc, s.modality, s.study_date, s.is_deleted "
                        "FROM patients p JOIN studies s ON p.patient_id = s.patient_id WHERE 1=1" + filters +
                        " ORDER BY s.study_date DESC LIMIT 100;";

    SQLExecDirect(hStmt, (SQLCHAR*)query.c_str(), SQL_NTS);

    // Bufory na kolejne kolumny wyniku. Rozmiary dobrane do typów w schemacie.
    SQLCHAR name[255], desc[255], mod[10], date[15], isDel[10];
    SQLLEN cbName, cbDesc, cbMod, cbDate, cbDel;
    int displayedCount = 0;

    std::cout << "\n--- WYNIKI WYSZUKIWANIA ---\n";
    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        // Odczyt 5 kolumn wiersza do buforów.
        SQLGetData(hStmt, 1, SQL_C_CHAR, name, sizeof(name), &cbName);
        SQLGetData(hStmt, 2, SQL_C_CHAR, desc, sizeof(desc), &cbDesc);
        SQLGetData(hStmt, 3, SQL_C_CHAR, mod, sizeof(mod), &cbMod);
        SQLGetData(hStmt, 4, SQL_C_CHAR, date, sizeof(date), &cbDate);
        SQLGetData(hStmt, 5, SQL_C_CHAR, isDel, sizeof(isDel), &cbDel);

        // is_deleted bywa reprezentowane różnie zależnie od sterownika/ustawień:
        // 't' (PostgreSQL boolean), '1' (ODBC) lub 'T'. Obsługujemy wszystkie warianty.
        std::string status = (isDel[0] == '1' || isDel[0] == 't' || isDel[0] == 'T') ? "[USUNIETY Z ARCHIWUM]" : "[DOSTEPNY]";
        std::cout << "Pacjent: " << name << " | Modalnosc: " << mod << " | Opis: " << desc << " | Data: " << date << " " << status << "\n";
        displayedCount++;
    }

    // Podsumowanie: pełna liczba trafień + informacja, jeśli pokazano tylko część.
    std::cout << "\nZnaleziono lacznie: " << totalFound << " badan.";
    if (totalFound > 100) {
        std::cout << " (Wyswietlono " << displayedCount << " najnowszych)";
    }
    std::cout << "\n---------------------------\n";

    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}


// =========================================================================
// OPERACJA 3 (Lekarz): STATYSTYKI SYSTEMU - wymaganie 5d ("prosta analiza").
// Cztery niezależne zapytania agregujące. Każde wykonujemy na tym samym uchwycie
// statement, zamykając kursor (SQLFreeStmt SQL_CLOSE) między nimi, by uchwyt był
// gotowy na kolejne zapytanie.
// =========================================================================
void showStatistics(SQLHDBC hDbc) {
    std::cout << "\n--- STATYSTYKI SYSTEMU ---\n";
    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);
    SQLCHAR result[255]; SQLLEN cbResult; // wspólny bufor na pojedynczą wartość wyniku

    // 1. Łączna liczba zarejestrowanych pacjentów.
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT COUNT(*) FROM patients;", SQL_NTS);
    if (SQLFetch(hStmt) == SQL_SUCCESS) { SQLGetData(hStmt, 1, SQL_C_CHAR, result, sizeof(result), &cbResult); std::cout << "Liczba zarejestrowanych pacjentow: " << result << "\n"; }
    SQLFreeStmt(hStmt, SQL_CLOSE);

    // 2. Liczba aktywnych plików (is_deleted=false) - pomijamy oznaczone jako usunięte.
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT COUNT(*) FROM studies WHERE is_deleted = false;", SQL_NTS);
    if (SQLFetch(hStmt) == SQL_SUCCESS) { SQLGetData(hStmt, 1, SQL_C_CHAR, result, sizeof(result), &cbResult); std::cout << "Liczba aktywnych plikow DICOM: " << result << "\n"; }
    SQLFreeStmt(hStmt, SQL_CLOSE);

    // 3. Rozmiar bazy na dysku - wbudowane funkcje PostgreSQL (pg_database_size +
    //    pg_size_pretty zwraca czytelny zapis typu "12 MB").
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT pg_size_pretty(pg_database_size('sim_medical_db'));", SQL_NTS);
    if (SQLFetch(hStmt) == SQL_SUCCESS) { SQLGetData(hStmt, 1, SQL_C_CHAR, result, sizeof(result), &cbResult); std::cout << "Wielkosc bazy danych SQL: " << result << "\n"; }
    SQLFreeStmt(hStmt, SQL_CLOSE);

    // 4. Rozkład wg modalności - GROUP BY zlicza badania w każdej grupie, ORDER BY
    //    sortuje od najliczniejszej. Tu wynik to WIELE wierszy, więc pętla while.
    std::cout << "Podzial wedlug modalnosci (aktywne):\n";
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT modality, COUNT(*) FROM studies WHERE is_deleted = false GROUP BY modality ORDER BY COUNT(*) DESC;", SQL_NTS);
    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLCHAR mod[10]; SQLGetData(hStmt, 1, SQL_C_CHAR, mod, sizeof(mod), &cbResult);     // kolumna 1: modalność
        SQLGetData(hStmt, 2, SQL_C_CHAR, result, sizeof(result), &cbResult);                 // kolumna 2: liczba
        std::cout << "  - " << mod << ": " << result << " plikow\n";
    }

    std::cout << "--------------------------\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}

// =========================================================================
// OPERACJA 1 (Lekarz): KSIĘGA PACJENTÓW - wymaganie 5d.
// Lista wszystkich pacjentów wraz z liczbą ich badań.
// =========================================================================
void listAllPatients(SQLHDBC hDbc) {
    std::cout << "\n--- KSIEGA PACJENTOW W SYSTEMIE ---\n";
    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // LEFT JOIN (a nie zwykły JOIN) gwarantuje, że pacjent bez badań również się pojawi
    // (z liczbą 0). GROUP BY + COUNT zliczają badania na pacjenta; LIMIT 100 chroni ekran.
    std::string query = "SELECT p.patient_id, p.full_name, COUNT(s.study_uid) "
                        "FROM patients p LEFT JOIN studies s ON p.patient_id = s.patient_id "
                        "GROUP BY p.patient_id, p.full_name ORDER BY p.full_name LIMIT 100;";

    SQLExecDirect(hStmt, (SQLCHAR*)query.c_str(), SQL_NTS);

    SQLCHAR id[128], name[255], count[10];
    SQLLEN cbId, cbName, cbCount;
    int total = 0;

    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLGetData(hStmt, 1, SQL_C_CHAR, id, sizeof(id), &cbId);
        SQLGetData(hStmt, 2, SQL_C_CHAR, name, sizeof(name), &cbName);
        SQLGetData(hStmt, 3, SQL_C_CHAR, count, sizeof(count), &cbCount);

        // Pacjent z pustym nazwiskiem (dane zanonimizowane) dostaje czytelny placeholder.
        std::string displayName = (std::string((char*)name) == "") ? "[ZANONIMIZOWANO]" : (char*)name;
        std::cout << "ID: " << id << " | Pacjent: " << displayName << " | Liczba zarchiwizowanych plikow: " << count << "\n";
        total++;
    }

    if (total == 0) std::cout << "Brak pacjentow w bazie.\n";
    std::cout << "-----------------------------------\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}
