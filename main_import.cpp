// =============================================================================
// main_import.cpp - PUNKT WEJŚCIA MODUŁU ZARZĄDCY (binarka: sim_importer)
//
// To jest "panel administratora" archiwum PACS. Odpowiada za WPROWADZANIE i
// utrzymanie danych: indeksowanie katalogów, import z kopiowaniem do archiwum
// oraz synchronizację bazy ze stanem dysku. Cała logika operacji znajduje się
// w db_handler.cpp - tutaj jest wyłącznie pętla menu rozdzielająca wybory.
//
// Świadomy podział ról: zarządca ma operacje MODYFIKUJĄCE bazę i archiwum,
// natomiast lekarz (sim_browser) dostaje osobną binarkę tylko do odczytu.
// =============================================================================

#include <iostream>
#include <string>
#include "db_handler.h"

int main() {
    std::cout << "Uruchamianie Modulu Importera SIM...\n";

    // Nawiązanie połączenia z bazą przez ODBC (DSN "SIM_DB"). Robimy to RAZ na
    // starcie i utrzymujemy przez cały czas działania menu, zamiast łączyć się
    // przy każdej operacji - to tańsze i upraszcza kod operacji w db_handler.cpp.
    SQLHDBC hDbc = connectToDatabase();
    if (!hDbc) return 1; // Brak połączenia = nie ma sensu uruchamiać menu; kod 1 sygnalizuje błąd.

    // Pętla menu działa, dopóki użytkownik nie wybierze "0".
    std::string choiceStr = "";
    while (choiceStr != "0") {
        std::cout << "\n=========================================\n";
        std::cout << "   SIM - MODUL ZARZADCY BAZY (IMPORTER)  \n";
        std::cout << "=========================================\n";
        std::cout << "1. Indeksuj katalog dyskowy (Bez kopiowania)\n";
        std::cout << "2. Importuj nowe badania (Kopiuj do Archiwum)\n";
        std::cout << "3. Synchronizuj baze (Sprawdz spojnosc)\n";
        std::cout << "0. Wyjscie z programu\n";
        std::cout << "Wybierz opcje: ";

        // Wybór czytamy jako całą linię przez getline, a nie przez (std::cin >> x).
        // Powód: getline nie zostawia w buforze znaku nowej linii, więc kolejne
        // getline-y w funkcjach operacji (pytające o ścieżki) działają poprawnie.
        // Dodatkowo obsługuje to wpisanie czegokolwiek bez wywracania programu.
        std::getline(std::cin, choiceStr);

        // Rozgałęzienie na konkretne operacje zaimplementowane w db_handler.cpp.
        if (choiceStr == "1") {
            indexDirectory(hDbc);   // Skan katalogu/pliku BEZ kopiowania (wymaganie 5c/5a)
        } else if (choiceStr == "2") {
            importDirectory(hDbc);  // Kopiowanie do ./SIM_ARCHIVE + zapis metadanych (wymaganie 5b)
        } else if (choiceStr == "3") {
            syncDatabase(hDbc);     // Dwukierunkowa synchronizacja baza<->dysk (wymaganie 5c)
        } else if (choiceStr == "0") {
            std::cout << "Zamykanie importera...\n";
        } else {
            // Każde inne wejście (literówka, puste Enter) nie kończy programu -
            // po prostu pokazujemy komunikat i wracamy na górę pętli.
            std::cout << "Nieznana opcja. Sprobuj ponownie.\n";
        }
    }

    // Porządne zwolnienie zasobów ODBC przed wyjściem.
    disconnectDatabase(hDbc);
    return 0;
}