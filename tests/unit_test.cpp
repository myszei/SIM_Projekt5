// =============================================================================
// TESTY JEDNOSTKOWE - czyste funkcje silnika SIM
//
// Strategia: dołączamy db_handler.cpp przez #include (nie ma w nim main()),
// dzięki czemu mamy dostęp do funkcji pomocniczych (trim, sanitizeForSQL,
// sanitizeForPath, safeDate, safeSex, removeDiacritics) BEZ modyfikowania kodu
// produkcyjnego. Funkcje ODBC z db_handler.cpp nie są wywoływane w testach,
// ale linkujemy -lodbc, bo są w jednostce kompilacji. extractDicomMetadata
// testujemy na realnych plikach z /var/dicom/archive.
//
// =============================================================================

#include "../db_handler.cpp"
#include "../dicom_handler.h"
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK_EQ(actual, expected, name)                                       \
    do {                                                                       \
        auto _a = (actual);                                                    \
        auto _e = (expected);                                                  \
        if (_a == _e) { ++g_pass; std::cout << "  [PASS] " << (name) << "\n"; }\
        else { ++g_fail; std::cout << "  [FAIL] " << (name)                    \
                 << "\n         oczekiwano: [" << _e << "]\n"                   \
                 << "         otrzymano:  [" << _a << "]\n"; }                  \
    } while (0)

#define CHECK_TRUE(cond, name)                                                 \
    do {                                                                       \
        if (cond) { ++g_pass; std::cout << "  [PASS] " << (name) << "\n"; }    \
        else { ++g_fail; std::cout << "  [FAIL] " << (name) << "\n"; }         \
    } while (0)

// helper: trim zwracający kopię (oryginał działa in-place)
static std::string trimmed(std::string s) { trim(s); return s; }

// =============================================================================
void test_trim() {
    std::cout << "== test_trim ==\n";
    CHECK_EQ(trimmed("  abc  "), std::string("abc"), "trim spacje obustronne");
    CHECK_EQ(trimmed("\t\nxy\r\n"), std::string("xy"), "trim taby/nowe linie");
    CHECK_EQ(trimmed("   "), std::string(""), "trim same biale znaki -> pusty");
    CHECK_EQ(trimmed(""), std::string(""), "trim pusty -> pusty");
    CHECK_EQ(trimmed("no_trim"), std::string("no_trim"), "trim bez bialych znakow");
    CHECK_EQ(trimmed("a b"), std::string("a b"), "trim zostawia spacje wewnetrzne");
}

void test_removeDiacritics() {
    std::cout << "== test_removeDiacritics ==\n";
    CHECK_EQ(removeDiacritics("Wiśniewski"), std::string("Wisniewski"), "polskie male znaki");
    CHECK_EQ(removeDiacritics("Zielińska"), std::string("Zielinska"), "polskie litery n");
    CHECK_EQ(removeDiacritics("ĄĆĘŁŃÓŚŹŻ"), std::string("ACELNOSZZ"), "polskie wielkie znaki");
    CHECK_EQ(removeDiacritics("Głowa"), std::string("Glowa"), "opis badania z l");
    CHECK_EQ(removeDiacritics("éüö"), std::string("euo"), "znaki zachodnioeuropejskie");
    CHECK_EQ(removeDiacritics("ASCII123"), std::string("ASCII123"), "czyste ASCII bez zmian");
}

void test_sanitizeForSQL() {
    std::cout << "== test_sanitizeForSQL ==\n";
    // Kluczowe dla bezpieczenstwa: apostrof -> podwojony apostrof
    CHECK_EQ(sanitizeForSQL("O'Brien"), std::string("O''Brien"), "apostrof podwojony");
    CHECK_EQ(sanitizeForSQL("' OR '1'='1"), std::string("'' OR ''1''=''1"),
             "proba SQL injection -> apostrofy zescapowane");
    CHECK_EQ(sanitizeForSQL("Wiśniewski"), std::string("Wisniewski"), "diakrytyki usuniete");
    CHECK_EQ(sanitizeForSQL("  abc  "), std::string("abc"), "trim na koncu");
    // znaki sterujace i nie-ASCII odrzucane
    CHECK_EQ(sanitizeForSQL(std::string("a\x01\x1f") + "b"), std::string("ab"),
             "znaki sterujace usuniete");
    CHECK_EQ(sanitizeForSQL(""), std::string(""), "pusty -> pusty");
}

void test_sanitizeForPath() {
    std::cout << "== test_sanitizeForPath ==\n";
    CHECK_EQ(sanitizeForPath("P001"), std::string("P001"), "alfanumeryczne bez zmian");
    CHECK_EQ(sanitizeForPath("Jan^Kowalski"), std::string("Jan_Kowalski"), "^ -> _");
    CHECK_EQ(sanitizeForPath("Klatka piersiowa"), std::string("Klatka_piersiowa"), "spacja -> _");
    CHECK_EQ(sanitizeForPath("Głowa"), std::string("Glowa"), "diakrytyk + alnum");
    CHECK_EQ(sanitizeForPath("a/b\\c:d*?"), std::string("abcd"),
             "niebezpieczne znaki sciezki usuniete");
    CHECK_EQ(sanitizeForPath("1.2.840.10"), std::string("1284010"),
             "kropki UID usuniete (zostaja cyfry)");
}

void test_safeDate() {
    std::cout << "== test_safeDate ==\n";
    CHECK_EQ(safeDate("20260115"), std::string("'20260115'"), "poprawna data DICOM 8 znakow");
    CHECK_EQ(safeDate("2026-01-15"), std::string("'2026-01-15'"), "data ISO z myslnikami (>=8)");
    CHECK_EQ(safeDate(""), std::string("NULL"), "pusta data -> NULL");
    CHECK_EQ(safeDate("2026"), std::string("NULL"), "za krotka data -> NULL");
    CHECK_EQ(safeDate("   "), std::string("NULL"), "biale znaki -> NULL");
    // safeDate najpierw escapuje apostrofy (sanitizeForSQL: ' -> ''), a nastepnie
    // owija calosc w pojedyncze apostrofy. Wejscie "' OR 1=1 --" (11 znakow, >=8)
    // daje literal '''  OR 1=1 --' = bezpieczny string, nie wykonalny SQL.
    CHECK_EQ(safeDate("' OR 1=1 --"), std::string("''' OR 1=1 --'"),
             "wstrzykniecie w date: apostrofy zescapowane (bezpieczne dla SQL)");
}

void test_safeSex() {
    std::cout << "== test_safeSex ==\n";
    CHECK_EQ(safeSex("M"), std::string("'M'"), "plec M");
    CHECK_EQ(safeSex("F"), std::string("'F'"), "plec F");
    CHECK_EQ(safeSex("O"), std::string("'O'"), "plec O (other)");
    CHECK_EQ(safeSex("m"), std::string("'M'"), "mala litera -> wielka");
    CHECK_EQ(safeSex(""), std::string("NULL"), "pusta plec -> NULL");
    CHECK_EQ(safeSex("Unknown"), std::string("NULL"), "smieci 'Unknown' -> NULL");
    CHECK_EQ(safeSex("X"), std::string("NULL"), "nieprawidlowa litera -> NULL");
}

void test_extractDicomMetadata() {
    std::cout << "== test_extractDicomMetadata ==\n";
    const std::string archive = "/var/dicom/archive";
    if (!fs::exists(archive)) {
        std::cout << "  [SKIP] brak " << archive << " (testy DICOM pominiete)\n";
        return;
    }

    // 1. Poprawny plik DICOM -> true + wypelnione kluczowe pola
    std::string firstValid;
    for (const auto& e : fs::recursive_directory_iterator(archive)) {
        if (!e.is_regular_file()) continue;
        DicomMetadata d;
        if (extractDicomMetadata(e.path().string(), d)) {
            firstValid = e.path().string();
            CHECK_TRUE(!d.patientId.empty(), "DICOM: patientId niepusty");
            CHECK_TRUE(!d.sopUid.empty(), "DICOM: sopUid niepusty");
            CHECK_TRUE(d.fileSize > 0, "DICOM: fileSize > 0");
            break;
        }
    }
    CHECK_TRUE(!firstValid.empty(), "DICOM: znaleziono >=1 poprawny plik");

    // 2. Plik nie-DICOM (tekstowy) -> false
    std::string tmp = "/tmp/sim_not_dicom.txt";
    { std::ofstream f(tmp); f << "to nie jest plik DICOM"; }
    DicomMetadata d2;
    CHECK_TRUE(!extractDicomMetadata(tmp, d2), "nie-DICOM (tekst) odrzucony");
    fs::remove(tmp);

    // 3. Pusty plik -> false
    std::string empty = "/tmp/sim_empty.dcm";
    { std::ofstream f(empty); }
    DicomMetadata d3;
    CHECK_TRUE(!extractDicomMetadata(empty, d3), "pusty plik odrzucony");
    fs::remove(empty);

    // 4. Nieistniejacy plik -> false
    DicomMetadata d4;
    CHECK_TRUE(!extractDicomMetadata("/tmp/nie_istnieje_12345.dcm", d4),
               "nieistniejacy plik odrzucony");
}

// =============================================================================
int main() {
    std::cout << "\n========== TESTY JEDNOSTKOWE SIM ==========\n\n";
    test_trim();
    test_removeDiacritics();
    test_sanitizeForSQL();
    test_sanitizeForPath();
    test_safeDate();
    test_safeSex();
    test_extractDicomMetadata();

    std::cout << "\n===========================================\n";
    std::cout << "PODSUMOWANIE: " << g_pass << " PASS, " << g_fail << " FAIL\n";
    std::cout << "===========================================\n";
    return g_fail == 0 ? 0 : 1;
}
