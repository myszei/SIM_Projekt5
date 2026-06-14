#include <iostream>
#include <string>
#include "db_handler.h"

int main() {
    std::cout << "Uruchamianie Modulu Importera SIM...\n";
    
    SQLHDBC hDbc = connectToDatabase();
    if (!hDbc) return 1;

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
        
        std::getline(std::cin, choiceStr); // Bezpieczne czytanie!

        if (choiceStr == "1") {
            indexDirectory(hDbc);
        } else if (choiceStr == "2") {
            importDirectory(hDbc);
        } else if (choiceStr == "3") {
            syncDatabase(hDbc);
        } else if (choiceStr == "0") {
            std::cout << "Zamykanie importera...\n";
        } else {
            std::cout << "Nieznana opcja. Sprobuj ponownie.\n";
        }
    }

    disconnectDatabase(hDbc);
    return 0;
}