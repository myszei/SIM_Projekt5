#include "dicom_handler.h"
#include "dcmtk/dcmdata/dctk.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Główna funkcja wyciągająca dane z pliku
bool extractDicomMetadata(const std::string& filePath, DicomMetadata& data) {
    DcmFileFormat fileformat;
    OFCondition status = fileformat.loadFile(filePath.c_str());
    if (!status.good()) {
        return false; 
    }
    
    DcmDataset *dataset = fileformat.getDataset();
    
    const char *pId = nullptr;
    const char *pName = nullptr;
    const char *pMod = nullptr;
    const char *pDate = nullptr;
    const char *pDesc = nullptr;
    const char *sUid = nullptr;

    const char *pBirthDate = nullptr;
    const char *pSex = nullptr;
    
    if (dataset->findAndGetString(DCM_PatientID, pId).good() && pId) data.patientId = pId;
    
    // Zabezpieczone imię pacjenta
    if (dataset->findAndGetString(DCM_PatientName, pName).good() && pName && *pName != '\0') {
        data.patientName = pName;
    } else {
        data.patientName = "BRAK DANYCH";
    }

    if (dataset->findAndGetString(DCM_Modality, pMod).good() && pMod) data.modality = pMod;
    if (dataset->findAndGetString(DCM_StudyDate, pDate).good() && pDate) data.studyDate = pDate;
    
    // --- Odczyt brakujących danych pacjenta ---
    if (dataset->findAndGetString(DCM_PatientBirthDate, pBirthDate).good() && pBirthDate) {
        data.birthDate = pBirthDate;
    }
    if (dataset->findAndGetString(DCM_PatientSex, pSex).good() && pSex) {
        data.patientSex = pSex;
    }
    // ----------------------------------------------------

    // Zabezpieczony opis 
    if (dataset->findAndGetString(DCM_StudyDescription, pDesc).good() && pDesc && *pDesc != '\0') {
        data.studyDesc = pDesc; 
    } else if (dataset->findAndGetString(DCM_SeriesDescription, pDesc).good() && pDesc && *pDesc != '\0') {
        data.studyDesc = pDesc; 
    } else if (dataset->findAndGetString(DCM_ProtocolName, pDesc).good() && pDesc && *pDesc != '\0') {
        data.studyDesc = pDesc; 
    } else {
        data.studyDesc = "BRAK_OPISU";
    }
    if (dataset->findAndGetString(DCM_SOPInstanceUID, sUid).good() && sUid) data.sopUid = sUid;
    
    // =========================================================================
    // Jeśli plik nie ma ID pacjenta lub UID badania, to NIE JEST poprawny DICOM
    // =========================================================================
    if (data.patientId.empty() || data.sopUid.empty()) {
        return false; 
    }
    
    data.filePath = filePath;
    data.fileSize = std::filesystem::file_size(filePath);
    return true;
}