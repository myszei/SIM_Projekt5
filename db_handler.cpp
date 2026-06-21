#include "db_handler.h"
#include "dicom_handler.h"
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// Funkcja pomocnicza do wyswietlania bledow SQL
void printOdbcError(SQLHANDLE handle, SQLSMALLINT type) {
    SQLCHAR sqlState[6], errorMsg[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER nativeError; SQLSMALLINT msgLen;
    if (SQLGetDiagRec(type, handle, 1, sqlState, &nativeError, errorMsg, sizeof(errorMsg), &msgLen) == SQL_SUCCESS) {
        std::cerr << "   [Baza Danych SQL Blad]: " << errorMsg << " (Stan: " << sqlState << ")\n";
    }
}

void trim(std::string& s) {
    if (s.empty()) return;
    std::size_t first = s.find_first_not_of(" \n\r\t");
    if (first == std::string::npos) { s.clear(); return; }
    std::size_t last = s.find_last_not_of(" \n\r\t");
    s = s.substr(first, last - first + 1);
}

// --- KROK 1: NARZĘDZIA DO CZYSZCZENIA DANYCH ---

void replaceAll(std::string& str, const std::string& from, const std::string& to) {
    if(from.empty()) return;
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); 
    }
}

// Podmienia znaki z innych alfabetów na ich bezpieczne odpowiedniki
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

std::string sanitizeForPath(std::string input) {
    input = removeDiacritics(input);
    std::string output;
    for (char c : input) {
        unsigned char uc = static_cast<unsigned char>(c);
        // Bezpieczne sprawdzanie czy znak to litera/cyfra
        if (isalnum(uc) || c == '-' || c == '_') {
            output += c;
        } else if (c == ' ' || c == '^') {
            output += '_';
        }
    }
    return output;
}

// Poprawiona funkcja dla bazy danych 
std::string sanitizeForSQL(const std::string& input) {
    std::string normalized = removeDiacritics(input);
    std::string output;
    for (char c : normalized) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc > 126) continue;
        else if (c == '\'') output += "''"; 
        else output += c;
    }
    trim(output);
    return output;
}

// =========================================================================
// BEZPIECZNA DATA (Odrzuca spacje i uszkodzone daty DICOM)
// =========================================================================
std::string safeDate(std::string dateStr) {
    dateStr = sanitizeForSQL(dateStr);
    // Poprawna data DICOM (DA) ma zawsze 8 znaków (YYYYMMDD). Jeśli jest krótsza/pusta, dajemy NULL.
    if (dateStr.empty() || dateStr.length() < 8) {
        return "NULL";
    }
    return "'" + dateStr + "'";
}

// =========================================================================
// BEZPIECZNA PŁEĆ (Wymusza max 1 znak dla SQL: M, F, O, w przeciwnym razie NULL)
// =========================================================================
std::string safeSex(std::string sexStr) {
    sexStr = sanitizeForSQL(sexStr);
    if (sexStr.empty()) return "NULL";
    
    char c = toupper(sexStr[0]); // Bierzemy tylko PIERWSZĄ literę i powiększamy
    if (c == 'M' || c == 'F' || c == 'O') {
        return std::string("'") + c + "'";
    }
    return "NULL"; // Jeśli to śmieci (np. "Unknown"), dajemy bezpieczny NULL
}

SQLHDBC connectToDatabase() {
    SQLHENV hEnv; SQLHDBC hDbc;
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

    if (!SQL_SUCCEEDED(SQLConnect(hDbc, (SQLCHAR*)"SIM_DB", SQL_NTS, (SQLCHAR*)"sim_user", SQL_NTS, (SQLCHAR*)"sim_password", SQL_NTS))) {
        std::cerr << "BLAD KRYTYCZNY: Brak polaczenia z baza przez ODBC!\n";
        return nullptr;
    }
    return hDbc;
}

void disconnectDatabase(SQLHDBC hDbc) {
    SQLDisconnect(hDbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
}

void indexDirectory(SQLHDBC hDbc) {
    std::string targetDir;
    std::cout << "\nPodaj sciezke do katalogu do ZAINDEKSOWANIA: ";
    std::getline(std::cin, targetDir);
    trim(targetDir);

    if (!fs::exists(targetDir)) { std::cerr << "BLAD: Katalog nie istnieje!\n"; return; }

    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    std::string sourceQuery = "INSERT INTO data_sources (directory_path, last_scanned) VALUES ('" + sanitizeForSQL(targetDir) + "', NOW()) RETURNING id;";
    if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)sourceQuery.c_str(), SQL_NTS))) {
        printOdbcError(hStmt, SQL_HANDLE_STMT);
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }
    long sourceId = 1; SQLLEN cbSourceId;
    SQLBindCol(hStmt, 1, SQL_C_LONG, &sourceId, 0, &cbSourceId); SQLFetch(hStmt); SQLFreeStmt(hStmt, SQL_CLOSE);

    int filesProcessed = 0;
    std::cout << "Indeksowanie plikow... prosze czekac.\n";

    for (const auto& entry : fs::recursive_directory_iterator(targetDir, fs::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file()) {
            DicomMetadata data;
            if (extractDicomMetadata(entry.path().string(), data)) {
                
                std::string s_id = sanitizeForSQL(data.patientId);
                std::string s_name = sanitizeForSQL(data.patientName);
                std::string s_uid = sanitizeForSQL(data.sopUid);
                std::string s_desc = sanitizeForSQL(data.studyDesc);
                
                // Używamy bezpiecznej daty i bezpiecznej płci
                std::string s_bDate = safeDate(data.birthDate);
                std::string s_sDate = safeDate(data.studyDate);
                std::string s_sex = safeSex(data.patientSex);
                std::string insertPatient = "INSERT INTO patients (patient_id, full_name, birth_date, sex) VALUES ('" + s_id + "', '" + s_name + "', " + s_bDate + ", " + s_sex + ") ON CONFLICT DO NOTHING;";
                
                if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertPatient.c_str(), SQL_NTS))) {
                    printOdbcError(hStmt, SQL_HANDLE_STMT);
                }

                std::string insertStudy = "INSERT INTO studies (study_uid, patient_id, source_id, modality, study_date, study_desc, file_path, file_size) VALUES ('" + s_uid + "', '" + s_id + "', " + std::to_string(sourceId) + ", '" + sanitizeForSQL(data.modality) + "', " + s_sDate + ", '" + s_desc + "', '" + sanitizeForSQL(data.filePath) + "', " + std::to_string(data.fileSize) + ") ON CONFLICT (study_uid) DO UPDATE SET is_deleted = false, file_path = EXCLUDED.file_path, source_id = EXCLUDED.source_id, study_desc = EXCLUDED.study_desc;";

                if (SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertStudy.c_str(), SQL_NTS))) {
                    filesProcessed++;
                } else {
                    printOdbcError(hStmt, SQL_HANDLE_STMT);
                }
            }
        }
    }
    std::cout << "SUKCES: Zaindeksowano plikow w bazie: " << filesProcessed << "\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}

void importDirectory(SQLHDBC hDbc) {
    std::string targetDir;
    std::cout << "\nPodaj zrodlo danych do IMPORTU (np. zewnetrzny dysk/pendrive): ";
    std::getline(std::cin, targetDir);
    trim(targetDir);

    if (!fs::exists(targetDir)) { std::cerr << "BLAD: Katalog nie istnieje!\n"; return; }

    std::string managedStorage = "./SIM_ARCHIVE";
    if (!fs::exists(managedStorage)) fs::create_directories(managedStorage);

    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    std::string sourceQuery = "INSERT INTO data_sources (directory_path, last_scanned) VALUES ('" + sanitizeForSQL(managedStorage) + "', NOW()) RETURNING id;";
    if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)sourceQuery.c_str(), SQL_NTS))) {
        printOdbcError(hStmt, SQL_HANDLE_STMT);
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }
    long sourceId = 1; SQLLEN cbSourceId;
    SQLBindCol(hStmt, 1, SQL_C_LONG, &sourceId, 0, &cbSourceId); SQLFetch(hStmt); SQLFreeStmt(hStmt, SQL_CLOSE);

    int filesProcessed = 0;
    std::cout << "Analiza i kopiowanie plikow do SIM_ARCHIVE...\n";

    for (const auto& entry : fs::recursive_directory_iterator(targetDir, fs::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file()) {
            std::string originalPath = entry.path().string();
            DicomMetadata data;
            
            if (extractDicomMetadata(originalPath, data)) {
                
                // 1. FOLDER GŁÓWNY PACJENTA (Tylko ID Pacjenta)
                std::string safePatientId = sanitizeForPath(data.patientId);
                if (safePatientId.empty()) safePatientId = "UNKNOWN_PATIENT"; // Zabezpieczenie
                
                std::string patientFolder = managedStorage + "/" + safePatientId;
                if (!fs::exists(patientFolder)) fs::create_directories(patientFolder);
                
                // 2. PODFOLDER BADANIA (Modalność + Opis)
                std::string safeModality = sanitizeForPath(data.modality.empty() ? "UNKNOWN" : data.modality);
                std::string safeDesc = sanitizeForPath(data.studyDesc.empty() ? "BRAK_OPISU" : data.studyDesc);
                
                // Struktura: np. MR_Guz_mozgu
                std::string studySubfolder = patientFolder + "/" + safeModality + "_" + safeDesc;
                if (!fs::exists(studySubfolder)) fs::create_directories(studySubfolder);
                
                // 3. FIZYCZNA ŚCIEŻKA DO PLIKU
                std::string destPath = studySubfolder + "/" + sanitizeForPath(data.sopUid) + ".dcm";
                std::string absoluteDest = fs::absolute(destPath).string();


                std::string s_id = sanitizeForSQL(data.patientId);
                std::string s_name = sanitizeForSQL(data.patientName);
                std::string s_uid = sanitizeForSQL(data.sopUid);
                std::string s_desc = sanitizeForSQL(data.studyDesc);
                
                // Zabezpieczenie DATY!
                std::string s_bDate = safeDate(data.birthDate);
                std::string s_sDate = safeDate(data.studyDate);
                std::string s_sex = safeSex(data.patientSex);
                std::string insertPatient = "INSERT INTO patients (patient_id, full_name, birth_date, sex) VALUES ('" + s_id + "', '" + s_name + "', " + s_bDate + ", " + s_sex + ") ON CONFLICT DO NOTHING;";
                
                if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertPatient.c_str(), SQL_NTS))) {
                    printOdbcError(hStmt, SQL_HANDLE_STMT);
                }
                
                std::string insertStudy = "INSERT INTO studies (study_uid, patient_id, source_id, modality, study_date, study_desc, file_path, file_size) VALUES ('" + s_uid + "', '" + s_id + "', " + std::to_string(sourceId) + ", '" + sanitizeForSQL(data.modality) + "', " + s_sDate + ", '" + s_desc + "', '" + sanitizeForSQL(absoluteDest) + "', " + std::to_string(data.fileSize) + ") ON CONFLICT (study_uid) DO UPDATE SET is_deleted = false, file_path = EXCLUDED.file_path, source_id = EXCLUDED.source_id, study_desc = EXCLUDED.study_desc;";

                if (SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)insertStudy.c_str(), SQL_NTS))) {
                    fs::copy_file(originalPath, destPath, fs::copy_options::overwrite_existing);
                    filesProcessed++;
                } else {
                    printOdbcError(hStmt, SQL_HANDLE_STMT);
                }
            }
        }
    }
    std::cout << "SUKCES: Fizycznie zabezpieczono i zapisano w bazie plikow: " << filesProcessed << "\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}

// =========================================================================
// DWUKIERUNKOWA SYNCHRONIZACJA (TWO-WAY SYNC)
// =========================================================================
void syncDatabase(SQLHDBC hDbc) {
    std::cout << "\n--- URUCHAMIANIE DWUKIERUNKOWEJ SYNCHRONIZACJI SYSTEMU ---\n";
    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // KROK 1: Z bazy na dysk (Oznaczanie usuniętych plików)
    std::string selectQuery = "SELECT study_uid, file_path FROM studies WHERE is_deleted = false;";
    SQLExecDirect(hStmt, (SQLCHAR*)selectQuery.c_str(), SQL_NTS);

    SQLCHAR uid[128], path[512]; SQLLEN cbUid, cbPath;
    std::vector<std::string> missingUids;

    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLGetData(hStmt, 1, SQL_C_CHAR, uid, sizeof(uid), &cbUid);
        SQLGetData(hStmt, 2, SQL_C_CHAR, path, sizeof(path), &cbPath);
        
        std::string checkPath = (char*)path;
        trim(checkPath); // CZYSZCZENIE ŚCIEŻKI

        // Jeśli plik nie istnieje (albo jest ścieżką sieciową Windowsa, którą wykasowano)
        if (!fs::exists(checkPath) || !fs::is_regular_file(checkPath)) {
            missingUids.push_back((char*)uid);
        }
    }
    SQLFreeStmt(hStmt, SQL_CLOSE);

    int markedDeleted = 0;
    for (const auto& mUid : missingUids) {
        std::string updateQuery = "UPDATE studies SET is_deleted = true WHERE study_uid = '" + mUid + "';";
        SQLExecDirect(hStmt, (SQLCHAR*)updateQuery.c_str(), SQL_NTS);
        markedDeleted++;
    }
    std::cout << "[Krok 1/2] Wykryto i oznaczono jako usuniete z dysku: " << markedDeleted << " badan.\n";

    // KROK 2: Z dysku do bazy (Wykrywanie plików wrzuconych ręcznie przez lekarza)
    std::string managedStorage = "./SIM_ARCHIVE";
    int autoRegistered = 0;

    if (fs::exists(managedStorage)) {
        std::cout << "[Krok 2/2] Skanowanie Archiwum w poszukiwaniu recznie dodanych plikow...\n";
        
        // Iterator omija zablokowane foldery systemowe
        for (const auto& entry : fs::recursive_directory_iterator(managedStorage, fs::directory_options::skip_permission_denied)) {
            
            // Program skanuje każdy plik, niezależnie od rozszerzenia (nawet te bez kropki)
            if (entry.is_regular_file()) { 
                std::string currentPath = fs::absolute(entry.path()).string();
                
                DicomMetadata data;
                // Otwieramy plik - jeśli to DICOM, wejdzie w warunek. NIfTI i inne zostaną zignorowane.
                if (extractDicomMetadata(currentPath, data)) {
                    std::string s_uid = sanitizeForSQL(data.sopUid);
                    
                    // Sprawdzamy w bazie, czy ten konkretny UID badania już istnieje
                    std::string checkQuery = "SELECT COUNT(*) FROM studies WHERE study_uid = '" + s_uid + "';";
                    SQLExecDirect(hStmt, (SQLCHAR*)checkQuery.c_str(), SQL_NTS);
                    
                    SQLCHAR countStr[20]; SQLLEN cbCount;
                    int exists = 0;
                    if (SQLFetch(hStmt) == SQL_SUCCESS) {
                        SQLGetData(hStmt, 1, SQL_C_CHAR, countStr, sizeof(countStr), &cbCount);
                        exists = std::stoi((char*)countStr);
                    }
                    SQLFreeStmt(hStmt, SQL_CLOSE);

                    // Jeśli pliku nie ma w bazie, rejestrujemy go
                    if (exists == 0) {
                        std::string s_id = sanitizeForSQL(data.patientId);
                        std::string s_name = sanitizeForSQL(data.patientName);
                        std::string s_desc = sanitizeForSQL(data.studyDesc);
                        
                        // Korzystamy z naszych bezpiecznych funkcji (zabezpieczenie przed błędami SQL)
                        std::string s_bDate = safeDate(data.birthDate);
                        std::string s_sDate = safeDate(data.studyDate);
                        std::string s_sex = safeSex(data.patientSex);

                        // Wklejamy pacjenta (zmienne safeDate i safeSex generują własne apostrofy lub czyste słowo NULL)
                        std::string insPat = "INSERT INTO patients (patient_id, full_name, birth_date, sex) VALUES ('" + s_id + "', '" + s_name + "', " + s_bDate + ", " + s_sex + ") ON CONFLICT DO NOTHING;";
                        SQLExecDirect(hStmt, (SQLCHAR*)insPat.c_str(), SQL_NTS);

                        // Wklejamy badanie
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
// 3. ZAAWANSOWANE WYSZUKIWANIE (KORZYSTA Z FUNKCJI TRIM NAPISANEJ WYŻEJ)
// =========================================================================
void searchPatient(SQLHDBC hDbc) {
    std::string phrase, modality, dateFrom, dateTo;
    
    std::cout << "\n--- ZAAWANSOWANE WYSZUKIWANIE BADAN ---\n";
    std::cout << "(Mozesz zostawic dowolne pole puste i wcisnac Enter)\n\n";

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

    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);
    
    // 1. BUDOWANIE WSPÓLNYCH FILTRÓW (Użyjemy ich dwukrotnie)
    std::string filters = "";
    if (!phrase.empty())   filters += " AND (p.full_name ILIKE '%" + phrase + "%' OR p.patient_id ILIKE '%" + phrase + "%')";
    if (!modality.empty()) filters += " AND s.modality ILIKE '%" + modality + "%'";
    if (!dateFrom.empty()) filters += " AND s.study_date >= '" + dateFrom + "'";
    if (!dateTo.empty())   filters += " AND s.study_date <= '" + dateTo + "'";

    // ==========================================================
    // KROK 1: SZYBKIE ZLICZENIE WSZYSTKICH PASUJĄCYCH WYNIKÓW
    // ==========================================================
    std::string countQuery = "SELECT COUNT(*) FROM patients p JOIN studies s ON p.patient_id = s.patient_id WHERE 1=1" + filters + ";";
    SQLExecDirect(hStmt, (SQLCHAR*)countQuery.c_str(), SQL_NTS);

    SQLCHAR totalStr[20]; SQLLEN cbTotal;
    int totalFound = 0;
    if (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLGetData(hStmt, 1, SQL_C_CHAR, totalStr, sizeof(totalStr), &cbTotal);
        totalFound = std::stoi((char*)totalStr);
    }
    SQLFreeStmt(hStmt, SQL_CLOSE); // Zamykamy kursor dla tego konkretnego zapytania

    // Jeśli nic nie ma, przerywamy od razu, nie ma sensu szukać dalej
    if (totalFound == 0) {
        std::cout << "\nBrak wynikow dla podanych kryteriow.\n---------------------------\n";
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return;
    }

    // ==========================================================
    // KROK 2: POBIERANIE WŁAŚCIWYCH DANYCH (Z LIMITEM 100)
    // ==========================================================
    std::string query = "SELECT p.full_name, s.study_desc, s.modality, s.study_date, s.is_deleted "
                        "FROM patients p JOIN studies s ON p.patient_id = s.patient_id WHERE 1=1" + filters + 
                        " ORDER BY s.study_date DESC LIMIT 100;";

    SQLExecDirect(hStmt, (SQLCHAR*)query.c_str(), SQL_NTS);

    SQLCHAR name[255], desc[255], mod[10], date[15], isDel[10];
    SQLLEN cbName, cbDesc, cbMod, cbDate, cbDel;
    int displayedCount = 0;

    std::cout << "\n--- WYNIKI WYSZUKIWANIA ---\n";
    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLGetData(hStmt, 1, SQL_C_CHAR, name, sizeof(name), &cbName);
        SQLGetData(hStmt, 2, SQL_C_CHAR, desc, sizeof(desc), &cbDesc);
        SQLGetData(hStmt, 3, SQL_C_CHAR, mod, sizeof(mod), &cbMod);
        SQLGetData(hStmt, 4, SQL_C_CHAR, date, sizeof(date), &cbDate);
        SQLGetData(hStmt, 5, SQL_C_CHAR, isDel, sizeof(isDel), &cbDel);

        // Sprawdzamy 't' (Linux), '1' (ODBC) oraz 'T' (zabezpieczenie)
        std::string status = (isDel[0] == '1' || isDel[0] == 't' || isDel[0] == 'T') ? "[USUNIETY Z ARCHIWUM]" : "[DOSTEPNY]";
        std::cout << "Pacjent: " << name << " | Modalnosc: " << mod << " | Opis: " << desc << " | Data: " << date << " " << status << "\n";
        displayedCount++;
    }
    
    // Piękne, profesjonalne podsumowanie na dole
    std::cout << "\nZnaleziono lacznie: " << totalFound << " badan.";
    if (totalFound > 100) {
        std::cout << " (Wyswietlono " << displayedCount << " najnowszych)";
    }
    std::cout << "\n---------------------------\n";
    
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}


// Generowanie statystyk z wykorzystaniem potęgi SQL
void showStatistics(SQLHDBC hDbc) {
    std::cout << "\n--- STATYSTYKI SYSTEMU ---\n";
    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);
    SQLCHAR result[255]; SQLLEN cbResult;

    // 1. Liczba pacjentów
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT COUNT(*) FROM patients;", SQL_NTS);
    if (SQLFetch(hStmt) == SQL_SUCCESS) { SQLGetData(hStmt, 1, SQL_C_CHAR, result, sizeof(result), &cbResult); std::cout << "Liczba zarejestrowanych pacjentow: " << result << "\n"; }
    SQLFreeStmt(hStmt, SQL_CLOSE);

    // 2. Liczba aktywnych plików
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT COUNT(*) FROM studies WHERE is_deleted = false;", SQL_NTS);
    if (SQLFetch(hStmt) == SQL_SUCCESS) { SQLGetData(hStmt, 1, SQL_C_CHAR, result, sizeof(result), &cbResult); std::cout << "Liczba aktywnych plikow DICOM: " << result << "\n"; }
    SQLFreeStmt(hStmt, SQL_CLOSE);

    // 3. Rozmiar bazy danych na dysku PostgreSQL
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT pg_size_pretty(pg_database_size('sim_medical_db'));", SQL_NTS);
    if (SQLFetch(hStmt) == SQL_SUCCESS) { SQLGetData(hStmt, 1, SQL_C_CHAR, result, sizeof(result), &cbResult); std::cout << "Wielkosc bazy danych SQL: " << result << "\n"; }
    SQLFreeStmt(hStmt, SQL_CLOSE);

    // 4. Rozkład modalności (Grupowanie SQL)
    std::cout << "Podzial wedlug modalnosci (aktywne):\n";
    SQLExecDirect(hStmt, (SQLCHAR*)"SELECT modality, COUNT(*) FROM studies WHERE is_deleted = false GROUP BY modality ORDER BY COUNT(*) DESC;", SQL_NTS);
    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        SQLCHAR mod[10]; SQLGetData(hStmt, 1, SQL_C_CHAR, mod, sizeof(mod), &cbResult);
        SQLGetData(hStmt, 2, SQL_C_CHAR, result, sizeof(result), &cbResult);
        std::cout << "  - " << mod << ": " << result << " plikow\n";
    }
    
    
    std::cout << "--------------------------\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}

// NOWOŚĆ: Wyświetlanie listy wszystkich pacjentów
void listAllPatients(SQLHDBC hDbc) {
    std::cout << "\n--- KSIEGA PACJENTOW W SYSTEMIE ---\n";
    SQLHSTMT hStmt; SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);
    
    // Pobieramy pacjentów i liczymy ile mają badań
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

        std::string displayName = (std::string((char*)name) == "") ? "[ZANONIMIZOWANO]" : (char*)name;
        std::cout << "ID: " << id << " | Pacjent: " << displayName << " | Liczba zarchiwizowanych plikow: " << count << "\n";
        total++;
    }
    
    if (total == 0) std::cout << "Brak pacjentow w bazie.\n";
    std::cout << "-----------------------------------\n";
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}
