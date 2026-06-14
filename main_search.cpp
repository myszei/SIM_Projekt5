#include <iostream>
#include <string>
#include "db_handler.h"

int main() {
    std::cout << "Uruchamianie Modulu Przegladarki SIM...\n";
    
    SQLHDBC hDbc = connectToDatabase();
    if (!hDbc) return 1;

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
        
        // Bezpieczne pobranie wyboru jako tekst - zero problemów z Enterem
        std::getline(std::cin, choiceStr);

        if (choiceStr == "1") {
            listAllPatients(hDbc);
        } else if (choiceStr == "2") {
            searchPatient(hDbc);
        } else if (choiceStr == "3") {
            showStatistics(hDbc);
        } else if (choiceStr == "0") {
            std::cout << "Zamykanie przegladarki...\n";
        } else {
            std::cout << "Nieznana opcja. Sprobuj ponownie.\n";
        }
    }

    disconnectDatabase(hDbc);
    return 0;
}