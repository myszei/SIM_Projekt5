// =============================================================================
// main_search.cpp - PUNKT WEJŚCIA MODUŁU LEKARZA (binarka: sim_browser)
//
// To jest "przeglądarka" dla lekarza/personelu klinicznego. W przeciwieństwie do
// modułu zarządcy (sim_importer) NIE modyfikuje danych - udostępnia wyłącznie
// operacje ODCZYTU: księgę pacjentów, zaawansowane wyszukiwanie badań oraz
// statystyki. Rozdzielenie na osobną binarkę realizuje zasadę najmniejszych
// uprawnień: lekarz nie ma jak przypadkiem zmienić zawartości archiwum.
//
// Cała logika zapytań jest w db_handler.cpp; tu znajduje się tylko pętla menu.
// =============================================================================

#include <iostream>
#include <string>
#include "db_handler.h"

int main() {
    std::cout << "Uruchamianie Modulu Przegladarki SIM...\n";

    // Połączenie z bazą przez ODBC (ten sam DSN "SIM_DB" co importer), nawiązane
    // raz na starcie. Bez połączenia nie ma czego przeglądać - kończymy z kodem 1.
    SQLHDBC hDbc = connectToDatabase();
    if (!hDbc) return 1;

    // Pętla menu działa do wyboru "0".
    std::string choiceStr = "";
    while (choiceStr != "0") {
        std::cout << "\n=========================================\n";
        std::cout << "   SIM - MODUL LEKARZA (PRZEGLADARKA)    \n";
        std::cout << "=========================================\n";
        std::cout << "1. Wyswietl ksiege pacjentow (Pokaz wszystkich)\n";
        std::cout << "2. Zaawansowane wyszukiwanie badan\n";
        std::cout << "3. Statystyki systemu\n";
        std::cout << "0. Wyjscie z programu\n";
        std::cout << "Wybierz opcje: ";

        // Czytamy całą linię (getline), bo funkcje wyszukiwania zadają potem
        // kolejne pytania getline-em - mieszanie z (cin >> x) psułoby bufor wejścia.
        std::getline(std::cin, choiceStr);

        // Rozgałęzienie na operacje odczytu z db_handler.cpp.
        if (choiceStr == "1") {
            listAllPatients(hDbc);  // Lista pacjentów + liczba badań każdego (wymaganie 5d)
        } else if (choiceStr == "2") {
            searchPatient(hDbc);    // Filtrowanie po nazwisku/ID/modalności/dacie (wymaganie 5d)
        } else if (choiceStr == "3") {
            showStatistics(hDbc);   // Prosta analiza statystyczna (wymaganie 5d)
        } else if (choiceStr == "0") {
            std::cout << "Zamykanie przegladarki...\n";
        } else {
            // Nieznany wybór nie kończy programu - pokazujemy menu ponownie.
            std::cout << "Nieznana opcja. Sprobuj ponownie.\n";
        }
    }

    // Zwolnienie uchwytów ODBC przed zakończeniem.
    disconnectDatabase(hDbc);
    return 0;
}