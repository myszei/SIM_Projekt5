// =============================================================================
// dicom_handler.cpp - WARSTWA ODCZYTU PLIKÓW DICOM
//
// Ten plik izoluje całą zależność od biblioteki DCMTK w jednym miejscu. Reszta
// systemu (db_handler.cpp) nie wie nic o formacie DICOM - operuje wyłącznie na
// gotowej, wypełnionej strukturze DicomMetadata. Dzięki temu, gdyby kiedyś
// zmieniła się biblioteka do czytania obrazów, modyfikacji wymagałby tylko ten plik.
// =============================================================================

#include "dicom_handler.h"
#include "dcmtk/dcmdata/dctk.h"   // DcmFileFormat, DcmDataset, tagi DCM_*
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Otwiera pojedynczy plik, próbuje odczytać z niego metadane DICOM i zapisuje je
// do struktury 'data'. Zwraca:
//   true  - plik to poprawny DICOM z minimalnym zestawem identyfikatorów,
//   false - plik nie jest DICOM-em / jest uszkodzony / brakuje kluczowych pól.
// Konwencja "false = pomiń ten plik" pozwala wywołującym (indeks/import/sync)
// bezpiecznie iterować po katalogach zawierających także pliki nie-DICOM.
bool extractDicomMetadata(const std::string& filePath, DicomMetadata& data) {
    // 1. Wczytanie pliku do pamięci przez DCMTK. loadFile() sam rozpoznaje, czy
    //    plik ma poprawny nagłówek DICOM (preambuła 128 bajtów + "DICM").
    DcmFileFormat fileformat;
    OFCondition status = fileformat.loadFile(filePath.c_str());
    if (!status.good()) {
        // Nie udało się wczytać - to nie jest plik DICOM (np. .txt, JPEG, plik pusty,
        // uszkodzony). Zwracamy false, żeby wywołujący po prostu go pominął.
        return false;
    }

    // getDataset() zwraca właściwy zbiór tagów (Data Set), z pominięciem
    // meta-nagłówka pliku. To na nim wykonujemy wszystkie odczyty findAndGetString.
    DcmDataset *dataset = fileformat.getDataset();

    // Wskaźniki, które DCMTK wypełni adresami wewnętrznych buforów łańcuchowych.
    // Inicjujemy je na nullptr, bo findAndGetString może ich nie ustawić, jeśli
    // tag nie istnieje - wtedy sprawdzenie "&& pX" chroni przed odczytem śmieci.
    const char *pId = nullptr;
    const char *pName = nullptr;
    const char *pMod = nullptr;
    const char *pDate = nullptr;
    const char *pDesc = nullptr;
    const char *sUid = nullptr;

    const char *pBirthDate = nullptr;
    const char *pSex = nullptr;

    // PatientID (0010,0020) - podstawowy identyfikator pacjenta, klucz w tabeli patients.
    // Warunek .good() && pId: tag istnieje ORAZ wskaźnik jest niepusty.
    if (dataset->findAndGetString(DCM_PatientID, pId).good() && pId) data.patientId = pId;

    // PatientName (0010,0010) - może być pusty w danych zanonimizowanych. Zamiast
    // zostawiać puste pole (które źle wygląda w wynikach), wstawiamy czytelny placeholder.
    // Dodatkowy warunek *pName != '\0' odrzuca przypadek "tag istnieje, ale jest pustym napisem".
    if (dataset->findAndGetString(DCM_PatientName, pName).good() && pName && *pName != '\0') {
        data.patientName = pName;
    } else {
        data.patientName = "BRAK DANYCH";
    }

    // Modality (0008,0060) - typ badania (CT, MR, CR, DX, US...). Kluczowe dla
    // filtrowania i statystyk po modalności.
    if (dataset->findAndGetString(DCM_Modality, pMod).good() && pMod) data.modality = pMod;
    // StudyDate (0008,0020) - data badania w formacie DICOM DA (YYYYMMDD).
    if (dataset->findAndGetString(DCM_StudyDate, pDate).good() && pDate) data.studyDate = pDate;

    // --- Dane demograficzne pacjenta (opcjonalne) ---
    // PatientBirthDate (0010,0030) i PatientSex (0010,0040). Mogą być nieobecne;
    // walidacją/normalizacją zajmą się funkcje safeDate()/safeSex() przy zapisie do bazy.
    if (dataset->findAndGetString(DCM_PatientBirthDate, pBirthDate).good() && pBirthDate) {
        data.birthDate = pBirthDate;
    }
    if (dataset->findAndGetString(DCM_PatientSex, pSex).good() && pSex) {
        data.patientSex = pSex;
    }
    // ----------------------------------------------------

    // Opis badania - kaskada źródeł. Różne aparaty wypełniają różne tagi, dlatego
    // próbujemy po kolei: StudyDescription -> SeriesDescription -> ProtocolName.
    // Bierzemy pierwszy NIEPUSTY (stąd warunek *pDesc != '\0'). Dzięki temu opis
    // jest możliwie sensowny niezależnie od tego, który tag wypełnił skaner.
    if (dataset->findAndGetString(DCM_StudyDescription, pDesc).good() && pDesc && *pDesc != '\0') {
        data.studyDesc = pDesc;
    } else if (dataset->findAndGetString(DCM_SeriesDescription, pDesc).good() && pDesc && *pDesc != '\0') {
        data.studyDesc = pDesc;
    } else if (dataset->findAndGetString(DCM_ProtocolName, pDesc).good() && pDesc && *pDesc != '\0') {
        data.studyDesc = pDesc;
    } else {
        // Żaden z tagów opisu nie był wypełniony - wstawiamy placeholder, żeby
        // nazwa podfolderu w archiwum i opis w bazie nigdy nie były puste.
        data.studyDesc = "BRAK_OPISU";
    }
    // SOPInstanceUID (0008,0018) - globalnie unikalny identyfikator pojedynczego
    // obrazu/instancji. To on jest kluczem głównym tabeli studies i podstawą
    // deduplikacji (ten sam obraz wczytany dwa razy nie utworzy dwóch rekordów).
    if (dataset->findAndGetString(DCM_SOPInstanceUID, sUid).good() && sUid) data.sopUid = sUid;

    // =========================================================================
    // WALIDACJA MINIMALNA: bez PatientID albo bez SOPInstanceUID nie da się
    // sensownie powiązać pliku z pacjentem ani zapisać unikalnego rekordu badania.
    // Taki plik traktujemy jak "nie-DICOM" i odrzucamy. To definicja "poprawnego
    // DICOM" w tym systemie i ostatnia bramka chroniąca bazę przed śmieciowymi wpisami.
    // =========================================================================
    if (data.patientId.empty() || data.sopUid.empty()) {
        return false;
    }

    // Komplet metadanych OK - dopisujemy informacje o samym pliku na dysku.
    data.filePath = filePath;
    data.fileSize = std::filesystem::file_size(filePath); // rozmiar w bajtach (kolumna file_size)
    return true;
}